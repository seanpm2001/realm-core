////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include <realm/object-store/results.hpp>

#include <realm/object-store/impl/realm_coordinator.hpp>
#include <realm/object-store/impl/results_notifier.hpp>
#include <realm/object-store/audit.hpp>
#include <realm/object-store/object_schema.hpp>
#include <realm/object-store/object_store.hpp>
#include <realm/object-store/schema.hpp>
#include <realm/object-store/class.hpp>
#include <realm/object-store/sectioned_results.hpp>
#include <realm/util/bson/bson.hpp>
#include <realm/set.hpp>

#include <stdexcept>

namespace realm {
[[noreturn]] static void unsupported_operation(ColKey column, Table const& table, const char* operation)
{
    auto type = ObjectSchema::from_core_type(column);
    std::string_view collection_type = column.is_collection() ? collection_type_name(column) : "property";
    const char* column_type = string_for_property_type(type & ~PropertyType::Collection);
    throw IllegalOperation(util::format("Operation '%1' not supported for %2%3 %4 '%5.%6'", operation, column_type,
                                        column.is_nullable() ? "?" : "", collection_type, table.get_class_name(),
                                        table.get_column_name(column)));
}

Results::Results() = default;
Results::~Results() = default;

Results::Results(SharedRealm r, Query q, DescriptorOrdering o)
    : m_realm(std::move(r))
    , m_query(std::move(q))
    , m_table(m_query.get_table())
    , m_table_view(m_table)
    , m_descriptor_ordering(std::move(o))
    , m_mode(Mode::Query)
    , m_mutex(m_realm && m_realm->is_frozen())
{
}

Results::Results(SharedRealm r, ConstTableRef table, const bson::BsonDocument& document)
    : Results(r, table->query(document))
{
}

Results::Results(SharedRealm r, ConstTableRef table, const std::string& document)
    : Results(r, table->query(static_cast<bson::BsonDocument>(bson::parse(document))))
{
}

Results::Results(const Class& cls)
    : Results(cls.get_realm(), cls.get_table())
{
}


Results::Results(SharedRealm r, ConstTableRef table)
    : m_realm(std::move(r))
    , m_table(table)
    , m_table_view(m_table)
    , m_mode(Mode::Table)
    , m_mutex(m_realm && m_realm->is_frozen())
{
}

Results::Results(std::shared_ptr<Realm> r, std::shared_ptr<CollectionBase> coll, util::Optional<Query> q,
                 SortDescriptor s)
    : m_realm(std::move(r))
    , m_table(coll->get_target_table())
    , m_collection(std::move(coll))
    , m_mode(Mode::Collection)
    , m_mutex(m_realm && m_realm->is_frozen())
{
    if (q) {
        m_query = std::move(*q);
        m_mode = Mode::Query;
    }
    m_descriptor_ordering.append_sort(std::move(s));
}

Results::Results(std::shared_ptr<Realm> r, std::shared_ptr<CollectionBase> coll, DescriptorOrdering o)
    : m_realm(std::move(r))
    , m_table(coll->get_target_table())
    , m_descriptor_ordering(std::move(o))
    , m_collection(std::move(coll))
    , m_mode(Mode::Collection)
    , m_mutex(m_realm && m_realm->is_frozen())
{
}

Results::Results(std::shared_ptr<Realm> r, TableView tv, DescriptorOrdering o)
    : m_realm(std::move(r))
    , m_table_view(std::move(tv))
    , m_descriptor_ordering(std::move(o))
    , m_mode(Mode::TableView)
    , m_mutex(m_realm && m_realm->is_frozen())
{
    m_table = m_table_view.get_parent();
}

Results::Results(const Results&) = default;
Results& Results::operator=(const Results&) = default;
Results::Results(Results&&) = default;
Results& Results::operator=(Results&&) = default;

Results::Mode Results::get_mode() const noexcept
{
    util::CheckedUniqueLock lock(m_mutex);
    return m_mode;
}

bool Results::is_valid() const
{
    if (m_realm) {
        m_realm->verify_thread();
    }

    // Here we cannot just use if (m_table) as it combines a check if the
    // reference contains a value and if that value is valid.
    // First we check if a table is referenced ...
    if (m_table.unchecked_ptr() != nullptr)
        return bool(m_table); // ... and then we check if it is valid

    if (m_collection)
        // Since m_table was not set, this is a collection of primitives
        // and the results validity depend directly on the collection
        return m_collection->is_attached();

    return true;
}

void Results::validate_read() const
{
    // is_valid ensures that we're on the correct thread.
    if (!is_valid())
        throw StaleAccessor("Access to invalidated Results objects");
}

void Results::validate_write() const
{
    validate_read();
    if (!m_realm || !m_realm->is_in_transaction())
        throw WrongTransactionState("Must be in a write transaction");
}

size_t Results::size()
{
    util::CheckedUniqueLock lock(m_mutex);
    return do_size();
}

size_t Results::do_size()
{
    validate_read();
    ensure_up_to_date(EvaluateMode::Count);
    switch (m_mode) {
        case Mode::Empty:
            return 0;
        case Mode::Table:
            return m_table ? m_table->size() : 0;
        case Mode::Collection:
            return m_list_indices ? m_list_indices->size() : m_collection->size();
        case Mode::Query:
            return m_query.count(m_descriptor_ordering);
        case Mode::TableView:
            return m_table_view.size();
    }
    REALM_COMPILER_HINT_UNREACHABLE();
}

const ObjectSchema& Results::get_object_schema() const
{
    validate_read();

    auto object_schema = m_object_schema.load();
    if (!object_schema) {
        REALM_ASSERT(m_realm);
        auto it = m_realm->schema().find(get_object_type());
        REALM_ASSERT(it != m_realm->schema().end());
        m_object_schema = object_schema = &*it;
    }

    return *object_schema;
}

StringData Results::get_object_type() const noexcept
{
    if (!m_table) {
        return StringData();
    }

    return ObjectStore::object_type_for_table_name(m_table->get_name());
}

bool Results::has_changed() REQUIRES(!m_mutex)
{
    util::CheckedUniqueLock lock(m_mutex);
    if (m_collection)
        return m_last_collection_content_version != m_collection->get_obj().get_table()->get_content_version();

    return m_table_view.has_changed();
}

void Results::ensure_up_to_date(EvaluateMode mode)
{
    if (m_update_policy == UpdatePolicy::Never) {
        REALM_ASSERT(m_mode == Mode::TableView);
        return;
    }

    switch (m_mode) {
        case Mode::Empty:
            return;
        case Mode::Table:
            // Tables are always up-to-date
            return;
        case Mode::Collection: {
            // Collections themselves are always up-to-date, but we may need
            // to apply sort descriptors
            if (m_descriptor_ordering.is_empty())
                return;

            // Collections of objects are sorted/distincted by converting them
            // to a TableView
            if (do_get_type() == PropertyType::Object) {
                m_query = do_get_query();
                m_mode = Mode::Query;
                ensure_up_to_date(mode);
                return;
            }

            // Other types we do manually via m_list_indices. Ideally we just
            // pull the updated one from the notifier, but we can't if it hasn't
            // run yet or if we're currently in a write transaction (as we can't
            // know if any relevant changes have happened so far in the write).
            if (m_notifier && m_notifier->get_list_indices(m_list_indices) && !m_realm->is_in_transaction())
                return;

            bool needs_update = m_collection->has_changed();
            if (!m_list_indices) {
                m_list_indices = std::vector<size_t>{};
                needs_update = true;
            }
            if (!needs_update)
                return;

            m_last_collection_content_version = m_collection->get_obj().get_table()->get_content_version();

            if (m_collection->is_empty()) {
                m_list_indices->clear();
                return;
            }

            // Note that for objects this would be wrong as .sort().distinct()
            // and distinct().sort() can pick different objects which have the
            // same value in the column being distincted, but that's not
            // applicable to non-objects. If there's two equal strings, it doesn't
            // matter which we pick.
            util::Optional<bool> sort_order;
            bool do_distinct = false;
            auto sz = m_descriptor_ordering.size();
            for (size_t i = 0; i < sz; i++) {
                auto descr = m_descriptor_ordering[i];
                if (descr->get_type() == DescriptorType::Sort)
                    sort_order = static_cast<const SortDescriptor*>(descr)->is_ascending(0);
                if (descr->get_type() == DescriptorType::Distinct)
                    do_distinct = true;
            }

            if (do_distinct)
                m_collection->distinct(*m_list_indices, sort_order);
            else if (sort_order)
                m_collection->sort(*m_list_indices, *sort_order);
            return;
        }

        case Mode::Query:
            // Everything except for size() requires evaluating the Query and
            // getting a TableView, and size() does as well if distinct is involved.
            if (mode == EvaluateMode::Count && !m_descriptor_ordering.will_apply_distinct()) {
                m_query.sync_view_if_needed();
                return;
            }

            // First we check if we ran the Query in the background and can
            // just use that
            if (m_notifier && m_notifier->get_tableview(m_table_view)) {
                m_mode = Mode::TableView;
                if (auto audit = m_realm->audit_context())
                    audit->record_query(m_realm->read_transaction_version(), m_table_view);
                return;
            }

            // We have to actually run the Query locally. We have an option
            // to disable this for testing purposes as it's otherwise very
            // difficult to determine if the async query is actually being
            // used.
            m_query.sync_view_if_needed();
            if (m_update_policy != UpdatePolicy::AsyncOnly)
                m_table_view = m_query.find_all(m_descriptor_ordering);
            m_mode = Mode::TableView;
            if (auto audit = m_realm->audit_context())
                audit->record_query(m_realm->read_transaction_version(), m_table_view);

            // Unless we're creating a snapshot, create an async notifier that'll
            // rerun this query in the background.
            if (mode != EvaluateMode::Snapshot && !m_notifier)
                prepare_async(ForCallback{false});
            return;

        case Mode::TableView:
            // Unless we're creating a snapshot, create an async notifier that'll
            // rerun this query in the background.
            if (mode != EvaluateMode::Snapshot && !m_notifier)
                prepare_async(ForCallback{false});
            // First check if we have an up-to-date TableView waiting for us
            // which was generated on the background thread
            else if (m_notifier)
                m_notifier->get_tableview(m_table_view);
            // This option is here so that tests can verify that the notifier
            // is actually being used.
            if (m_update_policy == UpdatePolicy::Auto)
                m_table_view.sync_if_needed();
            if (auto audit = m_realm->audit_context())
                audit->record_query(m_realm->read_transaction_version(), m_table_view);
            return;
    }
}

size_t Results::actual_index(size_t ndx) const noexcept
{
    if (auto& indices = m_list_indices) {
        return ndx < indices->size() ? (*indices)[ndx] : npos;
    }
    return ndx;
}

template <typename T>
static T get_unwraped(CollectionBase& collection, size_t ndx)
{
    using U = typename util::RemoveOptional<T>::type;
    Mixed mixed = collection.get_any(ndx);
    if (!mixed.is_null())
        return mixed.get<U>();
    return BPlusTree<T>::default_value(collection.get_col_key().is_nullable());
}

template <typename T>
util::Optional<T> Results::try_get(size_t ndx)
{
    validate_read();
    ensure_up_to_date();
    if (m_mode == Mode::Collection) {
        ndx = actual_index(ndx);
        if (ndx < m_collection->size()) {
            return get_unwraped<T>(*m_collection, ndx);
        }
    }
    return util::none;
}

Results::IteratorWrapper::IteratorWrapper(IteratorWrapper const& rgt)
{
    *this = rgt;
}

Results::IteratorWrapper& Results::IteratorWrapper::operator=(IteratorWrapper const& rgt)
{
    if (rgt.m_it)
        m_it = std::make_unique<Table::Iterator>(*rgt.m_it);
    return *this;
}

Obj Results::IteratorWrapper::get(Table const& table, size_t ndx)
{
    // Using a Table iterator is much faster for repeated access into a table
    // than indexing into it as the iterator caches the cluster the last accessed
    // object is stored in, but creating the iterator is somewhat expensive.
    if (!m_it) {
        if (table.size() <= 5)
            return const_cast<Table&>(table).get_object(ndx);
        m_it = std::make_unique<Table::Iterator>(table.begin());
    }
    m_it->go(ndx);
    return **m_it;
}

template <>
util::Optional<Obj> Results::try_get(size_t row_ndx)
{
    validate_read();
    ensure_up_to_date();
    switch (m_mode) {
        case Mode::Empty:
            break;
        case Mode::Table:
            if (m_table && row_ndx < m_table->size())
                return m_table_iterator.get(*m_table, row_ndx);
            break;
        case Mode::Collection:
            if (row_ndx < m_collection->size()) {
                auto m = m_collection->get_any(row_ndx);
                if (m.is_null())
                    return Obj();
                if (m.get_type() == type_Link)
                    return m_table->get_object(m.get<ObjKey>());
                if (m.get_type() == type_TypedLink)
                    return m_table->get_parent_group()->get_object(m.get_link());
            }
            break;
        case Mode::Query:
            REALM_UNREACHABLE();
        case Mode::TableView:
            if (row_ndx >= m_table_view.size())
                break;
            return m_table_view.get_object(row_ndx);
    }
    return util::none;
}

Mixed Results::get_any(size_t ndx)
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_read();
    ensure_up_to_date();
    switch (m_mode) {
        case Mode::Empty:
            break;
        case Mode::Table: {
            // Validity of m_table is checked in validate_read() above, so we
            // can skip all the checks here (which requires not using the
            // Mixed(Obj()) constructor)
            auto table = m_table.unchecked_ptr();
            if (ndx < table->size())
                return ObjLink(table->get_key(), m_table_iterator.get(*table, ndx).get_key());
            break;
        }
        case Mode::Collection:
            if (auto actual = actual_index(ndx); actual < m_collection->size())
                return m_collection->get_any(actual);
            break;
        case Mode::Query:
            REALM_UNREACHABLE(); // should always be in TV mode
        case Mode::TableView: {
            if (ndx >= m_table_view.size())
                break;
            if (m_update_policy == UpdatePolicy::Never && !m_table_view.is_obj_valid(ndx))
                return {};
            auto obj_key = m_table_view.get_key(ndx);
            return Mixed(ObjLink(m_table->get_key(), obj_key));
        }
    }
    throw OutOfBounds{"get_any() on Results", ndx, do_size()};
}

std::pair<StringData, Mixed> Results::get_dictionary_element(size_t ndx)
{
    util::CheckedUniqueLock lock(m_mutex);
    REALM_ASSERT(m_mode == Mode::Collection);
    auto& dict = static_cast<Dictionary&>(*m_collection);
    REALM_ASSERT(typeid(dict) == typeid(Dictionary));

    ensure_up_to_date();
    if (size_t actual = actual_index(ndx); actual < dict.size()) {
        auto val = dict.get_pair(ndx);
        return {val.first.get_string(), val.second};
    }
    throw OutOfBounds{"get_dictionary_element() on Results", ndx, dict.size()};
}

template <typename T>
T Results::get(size_t row_ndx)
{
    util::CheckedUniqueLock lock(m_mutex);
    if (auto row = try_get<T>(row_ndx)) {
        return *row;
    }
    throw OutOfBounds{"get() on Results", row_ndx, do_size()};
}

template <typename T>
util::Optional<T> Results::first()
{
    util::CheckedUniqueLock lock(m_mutex);
    return try_get<T>(0);
}

template <typename T>
util::Optional<T> Results::last()
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_read();
    if (m_mode == Mode::Query)
        ensure_up_to_date(); // avoid running the query twice (for size() and for get())
    return try_get<T>(do_size() - 1);
}

void Results::evaluate_query_if_needed(bool wants_notifications)
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_read();
    ensure_up_to_date(wants_notifications ? EvaluateMode::Normal : EvaluateMode::Snapshot);
}

template <>
size_t Results::index_of(Obj const& obj)
{
    if (!obj.is_valid()) {
        throw StaleAccessor{"Attempting to access an invalid object"};
    }
    if (m_table && obj.get_table() != m_table) {
        throw InvalidArgument(ErrorCodes::ObjectTypeMismatch,
                              util::format("Object of type '%1' does not match Results type '%2'",
                                           obj.get_table()->get_class_name(), m_table->get_class_name()));
    }
    return index_of(Mixed(obj.get_key()));
}

template <>
size_t Results::index_of(Mixed const& value)
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_read();
    ensure_up_to_date();

    if (value.is_type(type_TypedLink)) {
        if (m_table && m_table->get_key() != value.get_link().get_table_key()) {
            return realm::not_found;
        }
    }

    switch (m_mode) {
        case Mode::Empty:
        case Mode::Table:
            if (value.is_type(type_Link, type_TypedLink)) {
                return m_table->get_object_ndx(value.get<ObjKey>());
            }
            break;
        case Mode::Collection:
            if (m_list_indices) {
                for (size_t i = 0; i < m_list_indices->size(); ++i) {
                    if (value == m_collection->get_any(m_list_indices->at(i)))
                        return i;
                }
                return not_found;
            }
            return m_collection->find_any(value);
        case Mode::Query:
        case Mode::TableView:
            if (value.is_type(type_Link, type_TypedLink)) {
                return m_table_view.find_by_source_ndx(value.get<ObjKey>());
            }
            break;
    }
    return realm::not_found;
}

size_t Results::index_of(Query&& q)
{
    if (m_descriptor_ordering.will_apply_sort()) {
        Results filtered(filter(std::move(q)));
        filtered.assert_unlocked();
        auto first = filtered.first();
        return first ? index_of(*first) : not_found;
    }

    auto query = get_query().and_query(std::move(q));
    query.sync_view_if_needed();
    ObjKey row = query.find();
    return row ? index_of(const_cast<Table&>(*m_table).get_object(row)) : not_found;
}

namespace {
struct CollectionAggregateAdaptor {
    const CollectionBase& list;
    util::Optional<Mixed> min(ColKey)
    {
        return list.min();
    }
    util::Optional<Mixed> max(ColKey)
    {
        return list.max();
    }
    util::Optional<Mixed> sum(ColKey)
    {
        return list.sum();
    }
    util::Optional<Mixed> avg(ColKey)
    {
        return list.avg();
    }
};
} // anonymous namespace

template <typename AggregateFunction>
util::Optional<Mixed> Results::aggregate(ColKey column, const char* name, AggregateFunction&& func)
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_read();
    if (!m_table && !m_collection)
        return none;

    ensure_up_to_date();
    std::optional<Mixed> ret;
    switch (m_mode) {
        case Mode::Table:
            ret = func(*m_table);
            break;
        case Mode::Query:
            ret = func(m_query);
            break;
        case Mode::Collection:
            if (do_get_type() != PropertyType::Object)
                ret = func(CollectionAggregateAdaptor{*m_collection});
            else
                ret = func(do_get_query());
            break;
        default:
            ret = func(m_table_view);
            break;
    }

    // `none` indicates that it's an unsupported operation for the column type.
    // `some(null)` indicates that there's no rows in the thing being aggregated
    // Any other value is just the result to return
    if (ret) {
        return ret->is_null() ? std::nullopt : std::optional(*ret);
    }

    // We need to report the column and table actually being aggregated on,
    // which is the collection if it's not a link collection and the target
    // of the links otherwise
    if (m_mode == Mode::Collection && do_get_type() != PropertyType::Object) {
        unsupported_operation(m_collection->get_col_key(), *m_collection->get_table(), name);
    }
    else {
        unsupported_operation(column, *m_table, name);
    }
}

util::Optional<Mixed> Results::max(ColKey column)
{
    return aggregate(column, "max", [column](auto&& helper) {
        return helper.max(column);
    });
}

util::Optional<Mixed> Results::min(ColKey column)
{
    return aggregate(column, "min", [column](auto&& helper) {
        return helper.min(column);
    });
}

util::Optional<Mixed> Results::sum(ColKey column)
{
    return aggregate(column, "sum", [column](auto&& helper) {
        return helper.sum(column);
    });
}

util::Optional<Mixed> Results::average(ColKey column)
{
    return aggregate(column, "average", [column](auto&& helper) {
        return helper.avg(column);
    });
}

void Results::clear()
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_write();
    ensure_up_to_date();
    switch (m_mode) {
        case Mode::Empty:
            return;
        case Mode::Table:
            const_cast<Table&>(*m_table).clear();
            break;
        case Mode::Query:
            // Not using Query:remove() because building the tableview and
            // clearing it is actually significantly faster
        case Mode::TableView:
            switch (m_update_policy) {
                case UpdatePolicy::Auto:
                    m_table_view.clear();
                    break;
                case UpdatePolicy::AsyncOnly:
                case UpdatePolicy::Never: {
                    // Copy the TableView because a frozen Results shouldn't let its size() change.
                    TableView copy(m_table_view);
                    copy.clear();
                    break;
                }
            }
            break;
        case Mode::Collection:
            if (auto list = dynamic_cast<LnkLst*>(m_collection.get()))
                list->remove_all_target_rows();
            else if (auto set = dynamic_cast<LnkSet*>(m_collection.get()))
                set->remove_all_target_rows();
            else
                m_collection->clear();
            break;
    }
}

PropertyType Results::get_type() const
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_read();
    return do_get_type();
}

PropertyType Results::do_get_type() const
{
    switch (m_mode) {
        case Mode::Empty:
        case Mode::Query:
        case Mode::TableView:
        case Mode::Table:
            return PropertyType::Object;
        case Mode::Collection:
            return ObjectSchema::from_core_type(m_collection->get_col_key());
    }
    REALM_COMPILER_HINT_UNREACHABLE();
}

Query Results::get_query() const
{
    util::CheckedUniqueLock lock(m_mutex);
    return do_get_query();
}

const DescriptorOrdering& Results::get_ordering() const REQUIRES(!m_mutex)
{
    return m_descriptor_ordering;
}

ConstTableRef Results::get_table() const
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_read();
    switch (m_mode) {
        case Mode::Empty:
        case Mode::Query:
            return const_cast<Query&>(m_query).get_table();
        case Mode::TableView:
            return m_table_view.get_target_table();
        case Mode::Collection:
            return m_collection->get_target_table();
        case Mode::Table:
            return m_table;
    }
    REALM_COMPILER_HINT_UNREACHABLE();
}

Query Results::do_get_query() const
{
    validate_read();
    switch (m_mode) {
        case Mode::Empty:
        case Mode::Query:
        case Mode::TableView: {
            if (const_cast<Query&>(m_query).get_table())
                return m_query;

            // A TableView has an associated Query if it was produced by Query::find_all
            if (auto& query = m_table_view.get_query()) {
                return *query;
            }

            // The TableView has no associated query so create one with no conditions that is restricted
            // to the rows in the TableView.
            if (m_update_policy == UpdatePolicy::Auto) {
                m_table_view.sync_if_needed();
            }
            return Query(m_table, std::make_unique<TableView>(m_table_view));
        }
        case Mode::Collection:
            if (auto list = dynamic_cast<ObjList*>(m_collection.get())) {
                return m_table->where(*list);
            }
            if (auto dict = dynamic_cast<Dictionary*>(m_collection.get())) {
                if (dict->get_value_data_type() == type_Link) {
                    return m_table->where(*dict);
                }
            }
            return m_query;
        case Mode::Table:
            return m_table->where();
    }
    REALM_COMPILER_HINT_UNREACHABLE();
}

TableView Results::get_tableview()
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_read();
    ensure_up_to_date();
    switch (m_mode) {
        case Mode::Empty:
        case Mode::Collection:
            return do_get_query().find_all();
        case Mode::Query:
        case Mode::TableView:
            return m_table_view;
        case Mode::Table:
            return m_table->where().find_all();
    }
    REALM_COMPILER_HINT_UNREACHABLE();
}

static std::vector<ExtendedColumnKey> parse_keypath(StringData keypath, Schema const& schema,
                                                    const ObjectSchema* object_schema)
{
    auto check = [&](bool condition, const char* fmt, auto... args) {
        if (!condition) {
            throw InvalidArgument(
                util::format("Cannot sort on key path '%1': %2.", keypath, util::format(fmt, args...)));
        }
    };
    auto is_sortable_type = [](PropertyType type) {
        return !is_collection(type) && type != PropertyType::LinkingObjects && type != PropertyType::Data;
    };

    const char* begin = keypath.data();
    const char* end = keypath.data() + keypath.size();
    check(begin != end, "missing property name");

    std::vector<ExtendedColumnKey> indices;
    while (begin != end) {
        auto sep = std::find(begin, end, '.');
        check(sep != begin && sep + 1 != end, "missing property name");
        StringData key(begin, sep - begin);
        std::string index;
        auto begin_key = std::find(begin, sep, '[');
        if (begin_key != sep) {
            auto end_key = std::find(begin_key, sep, ']');
            check(end_key != sep, "missing ']'");
            index = std::string(begin_key + 1, end_key);
            key = StringData(begin, begin_key - begin);
        }
        begin = sep + (sep != end);

        auto prop = object_schema->property_for_public_name(key);
        check(prop, "property '%1.%2' does not exist", object_schema->name, key);
        if (is_dictionary(prop->type)) {
            check(index.length(), "missing dictionary key");
        }
        else {
            check(is_sortable_type(prop->type), "property '%1.%2' is of unsupported type '%3'", object_schema->name,
                  key, string_for_property_type(prop->type));
        }
        if (prop->type == PropertyType::Object)
            check(begin != end, "property '%1.%2' of type 'object' cannot be the final property in the key path",
                  object_schema->name, key);
        else
            check(begin == end, "property '%1.%2' of type '%3' may only be the final property in the key path",
                  object_schema->name, key, prop->type_string());

        if (index.length()) {
            indices.emplace_back(ColKey(prop->column_key), index);
        }
        else {
            indices.emplace_back(ColKey(prop->column_key));
        }
        if (prop->type == PropertyType::Object)
            object_schema = &*schema.find(prop->object_type);
    }
    return indices;
}

Results Results::sort(std::vector<std::pair<std::string, bool>> const& keypaths) const
{
    if (keypaths.empty())
        return *this;
    auto type = get_type();
    if (type != PropertyType::Object) {
        if (keypaths.size() != 1)
            throw InvalidArgument(util::format("Cannot sort array of '%1' on more than one key path",
                                               string_for_property_type(type & ~PropertyType::Flags)));
        if (keypaths[0].first != "self")
            throw InvalidArgument(
                util::format("Cannot sort on key path '%1': arrays of '%2' can only be sorted on 'self'",
                             keypaths[0].first, string_for_property_type(type & ~PropertyType::Flags)));
        return sort({{{}}, {keypaths[0].second}});
    }

    std::vector<std::vector<ExtendedColumnKey>> column_keys;
    std::vector<bool> ascending;
    column_keys.reserve(keypaths.size());
    ascending.reserve(keypaths.size());

    for (auto& keypath : keypaths) {
        column_keys.push_back(parse_keypath(keypath.first, m_realm->schema(), &get_object_schema()));
        ascending.push_back(keypath.second);
    }
    return sort({std::move(column_keys), std::move(ascending)});
}

Results Results::sort(SortDescriptor&& sort) const
{
    util::CheckedUniqueLock lock(m_mutex);
    DescriptorOrdering new_order = m_descriptor_ordering;
    new_order.append_sort(std::move(sort));
    if (m_mode == Mode::Collection)
        return Results(m_realm, m_collection, std::move(new_order));
    return Results(m_realm, do_get_query(), std::move(new_order));
}

Results Results::filter(Query&& q) const
{
    if (m_descriptor_ordering.will_apply_limit())
        throw IllegalOperation("Filtering a Results with a limit is not yet implemented");
    return Results(m_realm, get_query().and_query(std::move(q)), m_descriptor_ordering);
}

Results Results::find(const bson::BsonDocument& document) const
{
    return filter(m_table->query(document));
}

Results Results::find(const std::string& document) const
{
    return find(static_cast<bson::BsonDocument>(bson::parse(document)));
}

Results Results::limit(size_t max_count) const
{
    util::CheckedUniqueLock lock(m_mutex);
    auto new_order = m_descriptor_ordering;
    new_order.append_limit(max_count);
    if (m_mode == Mode::Collection)
        return Results(m_realm, m_collection, std::move(new_order));
    return Results(m_realm, do_get_query(), std::move(new_order));
}

Results Results::apply_ordering(DescriptorOrdering&& ordering)
{
    util::CheckedUniqueLock lock(m_mutex);
    DescriptorOrdering new_order = m_descriptor_ordering;
    new_order.append(std::move(ordering));
    if (m_mode == Mode::Collection)
        return Results(m_realm, m_collection, std::move(new_order));
    return Results(m_realm, do_get_query(), std::move(new_order));
}

Results Results::distinct(DistinctDescriptor&& uniqueness) const
{
    DescriptorOrdering new_order = m_descriptor_ordering;
    new_order.append_distinct(std::move(uniqueness));
    util::CheckedUniqueLock lock(m_mutex);
    if (m_mode == Mode::Collection)
        return Results(m_realm, m_collection, std::move(new_order));
    return Results(m_realm, do_get_query(), std::move(new_order));
}

Results Results::distinct(std::vector<std::string> const& keypaths) const
{
    if (keypaths.empty())
        return *this;
    auto type = get_type();
    if (type != PropertyType::Object) {
        if (keypaths.size() != 1)
            throw InvalidArgument(util::format("Cannot sort array of '%1' on more than one key path",
                                               string_for_property_type(type & ~PropertyType::Flags)));
        if (keypaths[0] != "self")
            throw InvalidArgument(
                util::format("Cannot sort on key path '%1': arrays of '%2' can only be sorted on 'self'", keypaths[0],
                             string_for_property_type(type & ~PropertyType::Flags)));
        return distinct(DistinctDescriptor({{ColKey()}}));
    }

    std::vector<std::vector<ExtendedColumnKey>> column_keys;
    column_keys.reserve(keypaths.size());
    for (auto& keypath : keypaths)
        column_keys.push_back(parse_keypath(keypath, m_realm->schema(), &get_object_schema()));
    return distinct({std::move(column_keys)});
}

Results Results::filter_by_method(std::function<bool(const Obj&)>&& predicate) const
{
    DescriptorOrdering new_order = m_descriptor_ordering;
    new_order.append_filter(FilterDescriptor(std::move(predicate)));
    util::CheckedUniqueLock lock(m_mutex);
    if (m_mode == Mode::Collection)
        return Results(m_realm, m_collection, std::move(new_order));
    return Results(m_realm, do_get_query(), std::move(new_order));
}

SectionedResults Results::sectioned_results(SectionedResults::SectionKeyFunc&& section_key_func) REQUIRES(m_mutex)
{
    return SectionedResults(*this, std::move(section_key_func));
}

SectionedResults Results::sectioned_results(SectionedResultsOperator op, util::Optional<StringData> prop_name)
    REQUIRES(m_mutex)
{
    return SectionedResults(*this, op, prop_name.value_or(StringData()));
}

Results Results::snapshot() const&
{
    validate_read();
    auto clone = *this;
    clone.assert_unlocked();
    return static_cast<Results&&>(clone).snapshot();
}

Results Results::snapshot() &&
{
    util::CheckedUniqueLock lock(m_mutex);
    validate_read();
    switch (m_mode) {
        case Mode::Empty:
            return Results();

        case Mode::Table:
        case Mode::Collection:
            m_query = do_get_query();
            if (m_query.get_table()) {
                m_mode = Mode::Query;
            }
            REALM_FALLTHROUGH;
        case Mode::Query:
        case Mode::TableView:
            ensure_up_to_date(EvaluateMode::Snapshot);
            m_notifier.reset();
            if (do_get_type() == PropertyType::Object) {
                m_update_policy = UpdatePolicy::Never;
            }
            return std::move(*this);
    }
    REALM_COMPILER_HINT_UNREACHABLE();
}

// This function cannot be called on frozen results and so does not require locking
void Results::prepare_async(ForCallback force) NO_THREAD_SAFETY_ANALYSIS
{
    REALM_ASSERT(m_realm);
    if (m_notifier)
        return;
    if (!m_realm->verify_notifications_available(force))
        return;
    if (m_update_policy == UpdatePolicy::Never) {
        if (force)
            throw LogicError(ErrorCodes::IllegalOperation,
                             "Cannot create asynchronous query for snapshotted Results.");
        return;
    }

    REALM_ASSERT(!force || !m_realm->is_frozen());
    if (!force) {
        // Don't do implicit background updates if we can't actually deliver them
        if (!m_realm->can_deliver_notifications())
            return;
        // Don't do implicit background updates if there isn't actually anything
        // that needs to be run.
        if (!m_query.get_table() && m_descriptor_ordering.is_empty())
            return;
    }

    if (do_get_type() != PropertyType::Object)
        m_notifier = std::make_shared<_impl::ListResultsNotifier>(*this);
    else
        m_notifier = std::make_shared<_impl::ResultsNotifier>(*this);
    _impl::RealmCoordinator::register_notifier(m_notifier);
}

NotificationToken Results::add_notification_callback(CollectionChangeCallback callback,
                                                     std::optional<KeyPathArray> key_path_array) &
{
    prepare_async(ForCallback{true});
    return {m_notifier, m_notifier->add_callback(std::move(callback), std::move(key_path_array))};
}

// This function cannot be called on frozen results and so does not require locking
bool Results::is_in_table_order() const NO_THREAD_SAFETY_ANALYSIS
{
    REALM_ASSERT(!m_realm || !m_realm->is_frozen());
    switch (m_mode) {
        case Mode::Empty:
        case Mode::Table:
            return true;
        case Mode::Collection:
            return false;
        case Mode::Query:
            return m_query.produces_results_in_table_order() && !m_descriptor_ordering.will_apply_sort();
        case Mode::TableView:
            return m_table_view.is_in_table_order();
    }
    REALM_COMPILER_HINT_UNREACHABLE();
}

ColKey Results::key(StringData name) const
{
    return m_table->get_column_key(name);
}
#define REALM_RESULTS_TYPE(T)                                                                                        \
    template T Results::get<T>(size_t);                                                                              \
    template util::Optional<T> Results::first<T>();                                                                  \
    template util::Optional<T> Results::last<T>();

REALM_RESULTS_TYPE(bool)
REALM_RESULTS_TYPE(int64_t)
REALM_RESULTS_TYPE(float)
REALM_RESULTS_TYPE(double)
REALM_RESULTS_TYPE(StringData)
REALM_RESULTS_TYPE(BinaryData)
REALM_RESULTS_TYPE(Timestamp)
REALM_RESULTS_TYPE(ObjectId)
REALM_RESULTS_TYPE(Decimal)
REALM_RESULTS_TYPE(UUID)
REALM_RESULTS_TYPE(Mixed)
REALM_RESULTS_TYPE(Obj)
REALM_RESULTS_TYPE(util::Optional<bool>)
REALM_RESULTS_TYPE(util::Optional<int64_t>)
REALM_RESULTS_TYPE(util::Optional<float>)
REALM_RESULTS_TYPE(util::Optional<double>)
REALM_RESULTS_TYPE(util::Optional<ObjectId>)
REALM_RESULTS_TYPE(util::Optional<UUID>)

#undef REALM_RESULTS_TYPE

Results Results::import_copy_into_realm(std::shared_ptr<Realm> const& realm)
{
    util::CheckedUniqueLock lock(m_mutex);
    if (m_mode == Mode::Empty)
        return *this;

    validate_read();

    switch (m_mode) {
        case Mode::Table:
            return Results(realm, realm->import_copy_of(m_table));
        case Mode::Collection:
            if (std::shared_ptr<CollectionBase> collection = realm->import_copy_of(*m_collection)) {
                return Results(realm, collection, m_descriptor_ordering);
            }
            // If collection is gone, fallback to empty selection on table.
            return Results(realm, TableView(realm->import_copy_of(m_table)));
            break;
        case Mode::Query:
            return Results(realm, *realm->import_copy_of(m_query, PayloadPolicy::Copy), m_descriptor_ordering);
        case Mode::TableView: {
            Results results(realm, *realm->import_copy_of(m_table_view, PayloadPolicy::Copy), m_descriptor_ordering);
            results.assert_unlocked();
            results.evaluate_query_if_needed(false);
            return results;
        }
        default:
            REALM_COMPILER_HINT_UNREACHABLE();
    }
}

Results Results::freeze(std::shared_ptr<Realm> const& frozen_realm)
{
    return import_copy_into_realm(frozen_realm);
}

bool Results::is_frozen() const
{
    return !m_realm || m_realm->is_frozen();
}

} // namespace realm
