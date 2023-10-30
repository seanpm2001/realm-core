/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <realm/table_view.hpp>
#include <realm/column_integer.hpp>
#include <realm/index_string.hpp>
#include <realm/transaction.hpp>

#include <unordered_set>

using namespace realm;

TableView::TableView(TableView& src, Transaction* tr, PayloadPolicy policy_mode)
    : m_source_column_key(src.m_source_column_key)
{
    bool was_in_sync = src.is_in_sync();
    if (src.m_query)
        m_query = Query(*src.m_query, tr, policy_mode);
    m_table = tr->import_copy_of(src.m_table);

    if (policy_mode == PayloadPolicy::Stay)
        was_in_sync = false;

    VersionID src_version =
        dynamic_cast<Transaction*>(src.m_table->get_parent_group())->get_version_of_current_transaction();
    if (src_version != tr->get_version_of_current_transaction())
        was_in_sync = false;

    m_table = tr->import_copy_of(src.m_table);
    m_collection_source = tr->import_copy_of(src.m_collection_source);
    if (src.m_source_column_key) {
        m_linked_obj = tr->import_copy_of(src.m_linked_obj);
    }

    if (was_in_sync)
        get_dependencies(m_last_seen_versions);

    // don't use methods which throw after this point...or m_table_view_key_values will leak
    if (policy_mode == PayloadPolicy::Copy && src.m_key_values.is_attached()) {
        m_key_values = src.m_key_values;
    }
    else if (policy_mode == PayloadPolicy::Move && src.m_key_values.is_attached())
        // Requires that 'src' is a writable object
        m_key_values = std::move(src.m_key_values);
    else {
        m_key_values.create();
    }
    if (policy_mode == PayloadPolicy::Move) {
        src.m_last_seen_versions.clear();
    }
    m_descriptor_ordering = src.m_descriptor_ordering;
    m_limit = src.m_limit;
}

// Aggregates ----------------------------------------------------

template <typename T, Action AggregateOpType>
struct Aggregator {
};

template <typename T>
struct Aggregator<T, act_Sum> {
    using AggType = typename aggregate_operations::Sum<typename util::RemoveOptional<T>::type>;
};

template <typename T>
struct Aggregator<T, act_Average> {
    using AggType = typename aggregate_operations::Average<typename util::RemoveOptional<T>::type>;
};

template <typename T>
struct Aggregator<T, act_Min> {
    using AggType = typename aggregate_operations::Minimum<typename util::RemoveOptional<T>::type>;
};

template <typename T>
struct Aggregator<T, act_Max> {
    using AggType = typename aggregate_operations::Maximum<typename util::RemoveOptional<T>::type>;
};

template <Action action, typename T>
Mixed TableView::aggregate(ColKey column_key, size_t* result_count, ObjKey* return_key) const
{
    static_assert(action == act_Sum || action == act_Max || action == act_Min || action == act_Average);
    REALM_ASSERT(m_table->valid_column(column_key));

    size_t non_nulls = 0;
    typename Aggregator<T, action>::AggType agg;
    ObjKey last_accumulated_key = null_key;
    for (size_t tv_index = 0; tv_index < m_key_values.size(); ++tv_index) {
        ObjKey key(get_key(tv_index));

        // skip detached references:
        if (key == realm::null_key)
            continue;

        const Obj obj = m_table->try_get_object(key);
        // aggregation must be robust in the face of stale keys:
        if (!obj.is_valid())
            continue;

        if (obj.is_null(column_key))
            continue;

        auto v = obj.get<T>(column_key);
        if (agg.accumulate(v)) {
            ++non_nulls;
            if constexpr (action == act_Min || action == act_Max) {
                last_accumulated_key = key;
            }
        }
    }

    if (result_count)
        *result_count = non_nulls;

    if constexpr (action == act_Max || action == act_Min) {
        if (return_key) {
            *return_key = last_accumulated_key;
        }
    }
    else {
        static_cast<void>(last_accumulated_key);
        static_cast<void>(return_key);
    }

    if (!agg.is_null()) {
        return agg.result();
    }
    if (action == act_Sum) {
        if (std::is_same_v<T, Mixed>) {
            return Decimal128{0};
        }
        return T{};
    }
    return Mixed();
}

template <Action action>
std::optional<Mixed> TableView::aggregate(ColKey column_key, size_t* count, ObjKey* return_key) const
{
    static_assert(action == act_Sum || action == act_Max || action == act_Min || action == act_Average);
    m_table->check_column(column_key);
    if (column_key.is_collection()) {
        return std::nullopt;
    }

    switch (column_key.get_type()) {
        case col_type_Int:
            if (m_table->is_nullable(column_key))
                return aggregate<action, util::Optional<int64_t>>(column_key, count, return_key);
            return aggregate<action, int64_t>(column_key, count, return_key);
        case col_type_Float:
            return aggregate<action, float>(column_key, count, return_key);
        case col_type_Double:
            return aggregate<action, double>(column_key, count, return_key);
        case col_type_Timestamp:
            if constexpr (action == act_Min || action == act_Max) {
                return aggregate<action, Timestamp>(column_key, count, return_key);
            }
            break;
        case col_type_Decimal:
            return aggregate<action, Decimal128>(column_key, count, return_key);
        case col_type_Mixed:
            return aggregate<action, Mixed>(column_key, count, return_key);
        default:
            break;
    }
    return util::none;
}

util::Optional<Mixed> TableView::min(ColKey column_key, ObjKey* return_key) const
{
    return aggregate<act_Min>(column_key, nullptr, return_key);
}

util::Optional<Mixed> TableView::max(ColKey column_key, ObjKey* return_key) const
{
    return aggregate<act_Max>(column_key, nullptr, return_key);
}

util::Optional<Mixed> TableView::sum(ColKey column_key) const
{
    return aggregate<act_Sum>(column_key, nullptr, nullptr);
}

util::Optional<Mixed> TableView::avg(ColKey column_key, size_t* value_count) const
{
    return aggregate<act_Average>(column_key, value_count, nullptr);
}

void TableView::to_json(std::ostream& out, size_t link_depth, const std::map<std::string, std::string>& renames,
                        JSONOutputMode mode) const
{
    // Represent table as list of objects
    out << "[";

    const size_t row_count = size();
    bool first = true;
    for (size_t r = 0; r < row_count; ++r) {
        if (ObjKey key = get_key(r)) {
            if (first) {
                first = false;
            }
            else {
                out << ",";
            }
            m_table->get_object(key).to_json(out, link_depth, renames, mode);
        }
    }

    out << "]";
}

bool TableView::depends_on_deleted_object() const
{
    if (m_collection_source) {
        return !m_collection_source->get_owning_obj().is_valid();
    }

    if (m_source_column_key && !m_linked_obj.is_valid()) {
        return true;
    }
    else if (m_query && m_query->m_source_table_view) {
        return m_query->m_source_table_view->depends_on_deleted_object();
    }
    return false;
}

void TableView::get_dependencies(TableVersions& ret) const
{
    auto table = m_table ? m_table.unchecked_ptr() : nullptr;
    if (m_source_column_key && m_linked_obj) {
        // m_source_column_key is set when this TableView was created by Table::get_backlink_view().
        if (auto linked_table = m_linked_obj.get_table()) {
            ret.emplace_back(linked_table->get_key(), linked_table->get_content_version());
        }
    }
    else if (m_query) {
        m_query->get_outside_versions(ret);
    }
    else {
        // This TableView was created by Table::get_distinct_view() or get_sorted_view() on collections
        ret.emplace_back(table->get_key(), table->get_content_version());
    }

    // Finally add dependencies from sort/distinct
    if (table) {
        m_descriptor_ordering.get_versions(table->get_parent_group(), ret);
    }
}

bool TableView::is_in_sync() const
{
    return m_table && !has_changed();
}

void TableView::sync_if_needed() const
{
    if (!is_in_sync()) {
        // FIXME: Is this a reasonable handling of constness?
        const_cast<TableView*>(this)->do_sync();
    }
}

void TableView::update_query(const Query& q)
{
    REALM_ASSERT(m_query);
    REALM_ASSERT(m_query->m_table);
    REALM_ASSERT(m_query->m_table == q.m_table);

    m_query = q;
    do_sync();
}

void TableView::clear()
{
    m_table.check();

    // If distinct is applied then removing an object may leave us out of sync
    // if there's another object with the same value in the distinct column
    // as the removed object
    bool sync_to_keep =
        m_last_seen_versions == get_dependency_versions() && !m_descriptor_ordering.will_apply_distinct();

    // Remove all invalid keys
    auto it = std::remove_if(m_key_values.begin(), m_key_values.end(), [this](const ObjKey& key) {
        return !m_table->is_valid(key);
    });
    m_key_values.erase(it, m_key_values.end());

    _impl::TableFriend::batch_erase_objects(*get_parent(), m_key_values); // Throws

    // It is important to not accidentally bring us in sync, if we were
    // not in sync to start with:
    if (sync_to_keep)
        m_last_seen_versions = get_dependency_versions();
}

void TableView::distinct(ColKey column)
{
    distinct(DistinctDescriptor({{column}}));
}

/// Remove rows that are duplicated with respect to the column set passed as argument.
/// Will keep original sorting order so that you can both have a distinct and sorted view.
void TableView::distinct(DistinctDescriptor columns)
{
    m_descriptor_ordering.append_distinct(std::move(columns));
    m_descriptor_ordering.collect_dependencies(m_table.unchecked_ptr());

    do_sync();
}

void TableView::limit(LimitDescriptor lim)
{
    m_descriptor_ordering.append_limit(std::move(lim));
    do_sync();
}

void TableView::filter(FilterDescriptor filter)
{
    m_descriptor_ordering.append_filter(std::move(filter));
    do_sync();
}

void TableView::apply_descriptor_ordering(const DescriptorOrdering& new_ordering)
{
    m_descriptor_ordering = new_ordering;
    m_descriptor_ordering.collect_dependencies(m_table.unchecked_ptr());

    do_sync();
}

std::string TableView::get_descriptor_ordering_description() const
{
    return m_descriptor_ordering.get_description(m_table);
}

// Sort according to one column
void TableView::sort(ColKey column, bool ascending)
{
    sort(SortDescriptor({{column}}, {ascending}));
}

// Sort according to multiple columns, user specified order on each column
void TableView::sort(SortDescriptor order)
{
    m_descriptor_ordering.append_sort(std::move(order), SortDescriptor::MergeMode::prepend);
    m_descriptor_ordering.collect_dependencies(m_table.unchecked_ptr());

    apply_descriptors(m_descriptor_ordering);
}


void TableView::do_sync()
{
    util::CriticalSection cs(m_race_detector);
    // This TableView can be "born" from 4 different sources:
    // - LinkView
    // - Query::find_all()
    // - Table::get_distinct_view()
    // - Table::get_backlink_view()
    // Here we sync with the respective source.
    m_last_seen_versions.clear();

    if (m_collection_source) {
        m_key_values.clear();
        auto sz = m_collection_source->size();
        for (size_t i = 0; i < sz; i++) {
            m_key_values.add(m_collection_source->get_key(i));
        }
    }
    else if (m_source_column_key) {
        m_key_values.clear();
        if (m_table && m_linked_obj.is_valid()) {
            if (m_table->valid_column(m_source_column_key)) { // return empty result, if column has been removed
                ColKey backlink_col = m_table->get_opposite_column(m_source_column_key);
                REALM_ASSERT(backlink_col);
                auto backlinks = m_linked_obj.get_all_backlinks(backlink_col);
                for (auto k : backlinks) {
                    m_key_values.add(k);
                }
            }
        }
    }
    // FIXME: Unimplemented for link to a column
    else {
        REALM_ASSERT(m_query);
        m_query->m_table.check();

        // valid query, so clear earlier results and reexecute it.
        if (m_key_values.is_attached())
            m_key_values.clear();
        else
            m_key_values.create();

        if (m_query->m_view)
            m_query->m_view->sync_if_needed();
        size_t limit = m_limit;
        if (!m_descriptor_ordering.is_empty()) {
            auto type = m_descriptor_ordering[0]->get_type();
            if (type == DescriptorType::Limit) {
                size_t l = static_cast<const LimitDescriptor*>(m_descriptor_ordering[0])->get_limit();
                if (l < limit)
                    limit = l;
            }
        }
        QueryStateFindAll<std::vector<ObjKey>> st(m_key_values, limit);
        m_query->do_find_all(st);
    }

    apply_descriptors(m_descriptor_ordering);

    get_dependencies(m_last_seen_versions);
}

void TableView::apply_descriptors(const DescriptorOrdering& ordering)
{
    if (ordering.is_empty())
        return;
    size_t sz = size();
    if (sz == 0)
        return;

    // Gather the current rows into a container we can use std algorithms on
    size_t detached_ref_count = 0;
    BaseDescriptor::IndexPairs index_pairs;
    bool using_indexpairs = false;

    auto apply_indexpairs = [&] {
        m_key_values.clear();
        for (auto& pair : index_pairs) {
            m_key_values.add(pair.key_for_object);
        }
        for (size_t t = 0; t < detached_ref_count; ++t)
            m_key_values.add(null_key);
        using_indexpairs = false;
    };

    auto use_indexpairs = [&] {
        index_pairs.reserve(sz);
        index_pairs.clear();
        // always put any detached refs at the end of the sort
        // FIXME: reconsider if this is the right thing to do
        // FIXME: consider specialized implementations in derived classes
        // (handling detached refs is not required in linkviews)
        for (size_t t = 0; t < sz; t++) {
            ObjKey key = get_key(t);
            if (m_table->is_valid(key)) {
                index_pairs.emplace_back(key, t);
            }
            else
                ++detached_ref_count;
        }
        using_indexpairs = true;
    };

    const int num_descriptors = int(ordering.size());
    for (int desc_ndx = 0; desc_ndx < num_descriptors; ++desc_ndx) {
        const BaseDescriptor* base_descr = ordering[desc_ndx];
        const BaseDescriptor* next = ((desc_ndx + 1) < num_descriptors) ? ordering[desc_ndx + 1] : nullptr;

        // Some descriptors, like Sort and Distinct, needs us to gather the current rows
        // into a container we can use std algorithms on
        if (base_descr->need_indexpair()) {
            if (!using_indexpairs) {
                use_indexpairs();
            }

            BaseDescriptor::Sorter predicate = base_descr->sorter(*m_table, index_pairs);

            // Sorting can be specified by multiple columns, so that if two entries in the first column are
            // identical, then the rows are ordered according to the second column, and so forth. For the
            // first column, we cache all the payload of fields of the view in a std::vector<Mixed>
            predicate.cache_first_column(index_pairs);

            base_descr->execute(index_pairs, predicate, next);
        }
        else {
            if (using_indexpairs) {
                apply_indexpairs();
            }
            base_descr->execute(*m_table, m_key_values, next);
            sz = size();
        }
    }
    // Apply the results
    if (using_indexpairs) {
        apply_indexpairs();
    }
}

bool TableView::is_in_table_order() const
{
    if (!m_table) {
        return false;
    }
    else if (m_collection_source) {
        return false;
    }
    else if (m_source_column_key) {
        return false;
    }
    else if (!m_query) {
        return false;
    }
    else {
        m_query->m_table.check();
        return m_query->produces_results_in_table_order() && !m_descriptor_ordering.will_apply_sort();
    }
}
