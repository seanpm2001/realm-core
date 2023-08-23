/*************************************************************************
 *
 * Copyright 2023 Realm Inc.
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

#include <realm/radix_tree.hpp>

#include <realm/column_integer.hpp>
#include <realm/utilities.hpp>

#include <iostream>

using namespace realm;
using namespace realm::util;

namespace {

void get_child(Array& parent, size_t child_ref_ndx, Array& child) noexcept
{
    ref_type child_ref = parent.get_as_ref(child_ref_ndx);
    child.init_from_ref(child_ref);
    child.set_parent(&parent, child_ref_ndx);
}

} // anonymous namespace

namespace realm {

size_t IndexIterator::num_matches() const
{
    if (!m_key) {
        return 0;
    }
    if (!m_list_position) {
        return 1;
    }
    REALM_ASSERT(last_node());
    REALM_ASSERT(m_positions.size());
    auto rot = last_node()->get_as_ref_or_tagged(m_positions.back());
    REALM_ASSERT_RELEASE_EX(rot.is_ref(), rot.get_as_int());
    const IntegerColumn sub(last_node()->get_alloc(), rot.get_as_ref()); // Throws
    const size_t sz = sub.size();
    REALM_ASSERT(sz);
    return sz - *m_list_position;
}

std::unique_ptr<IndexNode> IndexNode::create(Allocator& alloc)
{
    const Array::Type type = Array::type_HasRefs;
    std::unique_ptr<IndexNode> top = std::make_unique<IndexNode>(alloc); // Throws
    // Mark that this is part of index
    // (as opposed to columns under leaves)
    constexpr bool set_context_flag = true;
    constexpr int64_t initial_value = 0;
    top->Array::create(type, set_context_flag, c_num_metadata_entries, initial_value); // Throws
    top->ensure_minimum_width(0x7FFFFFFF); // This ensures 31 bits plus a sign bit

    // population is a tagged value
    top->set_population_0(0);
    top->set_population_1(0);
    return top;
}

inline bool value_can_be_tagged_without_overflow(uint64_t val)
{
    return !(val & (uint64_t(1) << 63));
}

template <size_t ChunkWidth>
std::unique_ptr<IndexNode> IndexNode::do_add_direct(ObjKey value, size_t ndx, IndexKey<ChunkWidth>& key)
{
    auto rot = get_as_ref_or_tagged(ndx);
    if (rot.is_tagged()) {
        // literal ObjKey here, split into a new list
        REALM_ASSERT(key.is_last());
        int64_t existing = rot.get_as_int();
        REALM_ASSERT_EX(existing != value.value, existing, value.value);
        // put these two entries into a new list
        Array row_list(m_alloc);
        row_list.create(Array::type_Normal); // Throws
        row_list.add(existing < value.value ? existing : value.value);
        row_list.add(existing < value.value ? value.value : existing);
        set(ndx, row_list.get_ref());
        update_parent();
        verify<ChunkWidth>();
        return nullptr;
    }
    ref_type ref = rot.get_as_ref();
    if (ref == 0) {
        REALM_ASSERT_3(ndx, ==, c_ndx_of_null);
        if (value_can_be_tagged_without_overflow(value.value)) {
            set(ndx, RefOrTagged::make_tagged(value.value));
        }
        else {
            // can't set directly because the high bit would be lost
            // add it to a list instead
            Array row_list(m_alloc);
            row_list.create(Array::type_Normal); // Throws
            row_list.add(value.value);
            set(ndx, row_list.get_ref());
        }
        verify<ChunkWidth>();
        return nullptr;
    }
    char* header = m_alloc.translate(ref);
    // ref to sorted list
    if (!Array::get_context_flag_from_header(header)) {
        REALM_ASSERT(key.is_last());
        IntegerColumn sub(m_alloc, ref); // Throws
        sub.set_parent(this, ndx);
        auto pos = sub.find_first(value.value);
        REALM_ASSERT_EX(pos == realm::npos || sub.get(pos) != value.value, pos, sub.size(), value.value);
        sub.insert(pos, value.value);
        verify<ChunkWidth>();
        return nullptr;
    }
    // ref to sub index node
    auto sub_node = std::make_unique<IndexNode>(m_alloc);
    sub_node->init_from_ref(ref);
    sub_node->set_parent(this, ndx);
    key.get_next();
    sub_node->verify<ChunkWidth>();
    verify<ChunkWidth>();
    return sub_node;
}

template <size_t ChunkWidth>
void IndexNode::insert(ObjKey value, IndexKey<ChunkWidth> key)
{
    init_from_parent();
    if (!key.get()) {
        auto has_nesting = do_add_direct(value, c_ndx_of_null, key);
        REALM_ASSERT(!has_nesting);
        return;
    }

    std::vector<std::unique_ptr<IndexNode>> accessor_chain;
    auto cur_node = std::make_unique<IndexNode>(m_alloc);
    cur_node->init_from_ref(this->get_ref());
    cur_node->set_parent(this->get_parent(), this->get_ndx_in_parent());
    cur_node->verify<ChunkWidth>();
    while (true) {
        InsertResult result = cur_node->insert_to_population(key);
        if (!result.did_exist) {
            // no entry for this yet, add one
            if (key.is_last()) {
                if (value_can_be_tagged_without_overflow(value.value)) {
                    // direct insert a single value
                    cur_node->Array::insert(result.real_index, RefOrTagged::make_tagged(value.value));
                }
                else {
                    // can't set directly because the high bit would be lost
                    // add it to a list instead
                    Array row_list(m_alloc);
                    row_list.create(Array::type_Normal); // Throws
                    row_list.add(value.value);
                    cur_node->set(result.real_index, row_list.get_ref());
                }
                cur_node->verify<ChunkWidth>();
                break;
            }
            else {
                auto new_node = create(m_alloc);
                cur_node->Array::insert(result.real_index, new_node->get_ref());
                new_node->set_parent(cur_node.get(), result.real_index);
                cur_node->verify<ChunkWidth>();
                key.get_next();
                accessor_chain.push_back(std::move(cur_node));
                cur_node = std::move(new_node);
            }
        }
        else {
            // entry already exists
            auto next = cur_node->do_add_direct(value, result.real_index, key);
            cur_node->verify<ChunkWidth>();
            if (!next) {
                break;
            }
            accessor_chain.push_back(std::move(cur_node));
            cur_node = std::move(next);
            cur_node->verify<ChunkWidth>();
        }
    }
}

bool IndexNode::do_remove(size_t raw_index)
{
    if (raw_index == c_ndx_of_null) {
        Array::set(c_ndx_of_null, 0);
        return is_empty();
    }
    Array::erase(raw_index);

    uint64_t population_0 = get_population_0();
    uint64_t population_1 = get_population_1();
    REALM_ASSERT(population_0 || population_1);
    REALM_ASSERT(raw_index >= c_num_metadata_entries);
    // count population prefix zeros to find starting index
    size_t bit_n = raw_index - c_num_metadata_entries;
    size_t index_translated;
    size_t bits_in_population_0 = size_t(fast_popcount64(population_0));
    if (bits_in_population_0 > bit_n) {
        index_translated = index_of_nth_bit(population_0, bit_n);
        REALM_ASSERT_EX(population_0 & (uint64_t(1) << index_translated), population_0, index_translated);
        population_0 = population_0 & ~(uint64_t(1) << index_translated);
        set_population_0(population_0);
    }
    else {
        index_translated = index_of_nth_bit(population_1, bit_n - bits_in_population_0);
        REALM_ASSERT_EX(population_1 & (uint64_t(1) << index_translated), population_1, index_translated);
        population_1 = population_1 & ~(uint64_t(1) << index_translated);
        set_population_1(population_1);
    }
    return population_0 == 0 && population_1 == 0 && get(c_ndx_of_null) == 0;
}

template <size_t ChunkWidth>
void IndexNode::erase(ObjKey value, IndexKey<ChunkWidth> key)
{
    init_from_parent();
    IndexIterator it = find_first(key);
    REALM_ASSERT_EX(it, value, key.get_mixed());
    REALM_ASSERT_EX(it.m_positions.size(), value, key.get_mixed());
    REALM_ASSERT_EX(it.last_node(), value, key.get_mixed());

    bool collapse_node = false;
    if (it.m_list_position) {
        auto rot = it.last_node()->get_as_ref_or_tagged(it.m_positions.back());
        REALM_ASSERT(rot.is_ref());
        IntegerColumn sub(m_alloc, rot.get_as_ref()); // Throws
        sub.set_parent(it.last_node(), it.m_positions.back());
        REALM_ASSERT_3(sub.size(), >, *it.m_list_position);
        auto ndx_in_list = sub.find_first(value.value);
        REALM_ASSERT(ndx_in_list != realm::not_found);
        REALM_ASSERT_3(sub.get(ndx_in_list), ==, value.value);
        sub.erase(ndx_in_list);
        if (sub.is_empty()) {
            sub.destroy();
            collapse_node = it.last_node()->do_remove(it.m_positions.back());
        }
    }
    else {
        // not a list, just a tagged ObjKey
        REALM_ASSERT_3(it.last_node()->size(), >, it.m_positions.back());
        collapse_node = it.last_node()->do_remove(it.m_positions.back());
    }
    if (collapse_node) {
        REALM_ASSERT(it.last_node()->is_empty());
        while (it.m_accessor_chain.size() > 1) {
            std::unique_ptr<IndexNode> cur_node;
            std::swap(cur_node, it.m_accessor_chain.back());
            it.m_accessor_chain.pop_back();
            if (!cur_node->is_empty()) {
                break;
            }
            it.m_accessor_chain.back()->do_remove(cur_node->get_ndx_in_parent());
            cur_node->destroy();
        }
    }
}

template <size_t ChunkWidth>
IndexIterator IndexNode::find_first(IndexKey<ChunkWidth> key) const
{
    IndexIterator ret;
    std::unique_ptr<IndexNode> cur_node = std::make_unique<IndexNode>(m_alloc);
    cur_node->init_from_ref(this->get_ref());
    cur_node->set_parent(get_parent(), get_ndx_in_parent());

    // search for nulls in the root
    if (!key.get()) {
        auto rot = get_as_ref_or_tagged(c_ndx_of_null);
        if (rot.is_ref()) {
            ref_type ref = rot.get_as_ref();
            if (!ref) {
                return {}; // no nulls
            }
            const IntegerColumn sub(m_alloc, ref); // Throws
            REALM_ASSERT(sub.size());
            ret.m_positions = {c_ndx_of_null};
            ret.m_accessor_chain.push_back(std::move(cur_node));
            ret.m_list_position = 0;
            ret.m_key = ObjKey(sub.get(0));
            return ret;
        }
        ret.m_positions = {c_ndx_of_null};
        ret.m_accessor_chain.push_back(std::move(cur_node));
        ret.m_key = ObjKey(rot.get_as_int());
        return ret;
    }

    while (true) {
        auto cur_prefix = cur_node->get_prefix<ChunkWidth>();
        for (auto prefix_chunk : cur_prefix) {
            auto key_chunk = key.get();
            if (!key_chunk || *key_chunk != size_t(prefix_chunk.to_ullong())) {
                return {}; // not found
            }
            key.next();
        }
        std::optional<size_t> ndx = cur_node->index_of(key);
        if (!ndx) {
            return {}; // no index entry
        }
        ret.m_positions.push_back(*ndx);
        ret.m_accessor_chain.push_back(std::move(cur_node));
        IndexNode* node = ret.m_accessor_chain.back().get();
        auto rot = node->get_as_ref_or_tagged(*ndx);
        if (rot.is_tagged()) {
            REALM_ASSERT(key.is_last());
            ret.m_key = ObjKey(rot.get_as_int());
            return ret;
        }
        else {
            ref_type ref = rot.get_as_ref();
            char* header = m_alloc.translate(ref);
            // ref to sorted list
            if (!Array::get_context_flag_from_header(header)) {
                REALM_ASSERT(key.is_last());
                const IntegerColumn sub(m_alloc, ref); // Throws
                REALM_ASSERT(sub.size());
                ret.m_key = ObjKey(sub.get(0));
                ret.m_list_position = 0;
                return ret;
            }
            else {
                key.get_next();
                auto sub_node = std::make_unique<IndexNode>(m_alloc);
                sub_node->init_from_ref(ref);
                sub_node->set_parent(node, *ndx);
                cur_node = std::move(sub_node);
                continue;
            }
        }
    }
    return ret;
}

template <size_t ChunkWidth>
void IndexNode::find_all(std::vector<ObjKey>& results, IndexKey<ChunkWidth> key) const
{
    IndexIterator it = find_first(key);
    if (!it) {
        return;
    }
    if (!it.m_list_position) {
        results.push_back(it.get_key());
        return;
    }
    REALM_ASSERT(it.m_positions.size());
    auto rot = it.last_node()->get_as_ref_or_tagged(it.m_positions.back());
    REALM_ASSERT(rot.is_ref());
    ref_type ref = rot.get_as_ref();
    char* header = m_alloc.translate(ref);
    REALM_ASSERT_RELEASE(!Array::get_context_flag_from_header(header));
    const IntegerColumn sub(m_alloc, rot.get_as_ref()); // Throws
    REALM_ASSERT(sub.size());
    for (auto sub_it = sub.cbegin(); sub_it != sub.cend(); ++sub_it) {
        results.push_back(ObjKey(*sub_it));
    }
}

template <size_t ChunkWidth>
FindRes IndexNode::find_all_no_copy(IndexKey<ChunkWidth> value, InternalFindResult& result) const
{
    IndexIterator it = find_first(value);
    if (!it) {
        return FindRes::FindRes_not_found;
    }
    if (!it.m_list_position) {
        result.payload = it.get_key().value;
        return FindRes::FindRes_single;
    }
    REALM_ASSERT(it.m_positions.size());
    auto rot = it.last_node()->get_as_ref_or_tagged(it.m_positions.back());
    REALM_ASSERT(rot.is_ref());
    ref_type ref = rot.get_as_ref();
    char* header = m_alloc.translate(ref);
    REALM_ASSERT_RELEASE(!Array::get_context_flag_from_header(header));
    const IntegerColumn sub(m_alloc, rot.get_as_ref()); // Throws
    REALM_ASSERT(sub.size());
    result.payload = rot.get_as_ref();
    result.start_ndx = *it.m_list_position;
    result.end_ndx = sub.size();
    return FindRes::FindRes_column;
}

void IndexNode::clear()
{
    init_from_parent();
    this->truncate_and_destroy_children(c_num_metadata_entries);
    RefOrTagged rot = get_as_ref_or_tagged(c_ndx_of_null);
    if (rot.is_ref() && rot.get_as_ref() != 0) {
        destroy_deep(rot.get_as_ref(), m_alloc);
    }
    set(c_ndx_of_null, 0);
    set_population_0(0);
    set_population_1(0);
}

uint64_t IndexNode::get_population_0() const
{
    return uint64_t(get(c_ndx_of_population_0)) >> 1;
}

uint64_t IndexNode::get_population_1() const
{
    return uint64_t(get(c_ndx_of_population_1)) >> 1;
}

void IndexNode::set_population_0(uint64_t pop)
{
    set(c_ndx_of_population_0, RefOrTagged::make_tagged(pop));
}

void IndexNode::set_population_1(uint64_t pop)
{
    set(c_ndx_of_population_1, RefOrTagged::make_tagged(pop));
}

bool IndexNode::has_prefix() const
{
    return get(c_ndx_of_prefix_payload) != 0;
}

template <size_t ChunkWidth>
typename IndexKey<ChunkWidth>::Prefix IndexKey<ChunkWidth>::advance_chunks(size_t num_chunks)
{
    IndexKey<ChunkWidth>::Prefix prefix{};
    while (!is_last() && prefix.size() < num_chunks) {
        auto next_chunk = get();
        REALM_ASSERT(next_chunk);
        prefix.push_back(std::bitset<ChunkWidth>(*next_chunk));
        next();
    }
    return prefix;
}

template <size_t ChunkWidth>
typename IndexKey<ChunkWidth>::Prefix IndexNode::get_prefix() const
{

    typename IndexKey<ChunkWidth>::Prefix prefix{};
    RefOrTagged rot_size = get_as_ref_or_tagged(c_ndx_of_prefix_size);
    size_t num_chunks = 0;
    if (rot_size.is_ref()) {
        REALM_ASSERT_3(rot_size.get_as_ref(), ==, 0);
    }
    else if (rot_size.is_tagged()) {
        num_chunks = rot_size.get_as_int();
    }
    if (num_chunks == 0) {
        return prefix;
    }
    auto unpack_prefix = [&prefix](int64_t packed_value) {
        IndexKey<ChunkWidth> compact_prefix(packed_value);
        for (size_t i = 0; i < IndexKey<ChunkWidth>::c_key_chunks_per_prefix; ++i) {
            auto chunk = compact_prefix.get();
            REALM_ASSERT(chunk);
            prefix.push_back(*chunk);
            compact_prefix.next();
        }
    };

    RefOrTagged rot_payload = get_as_ref_or_tagged(c_ndx_of_prefix_payload);
    if (rot_payload.is_ref()) {
        ref_type ref = rot_payload.get_as_ref();
        if (ref == 0) {
            REALM_ASSERT_3(prefix.size(), <, IndexKey<ChunkWidth>::c_key_chunks_per_prefix);
            for (size_t i = 0; i < num_chunks; ++i) {
                prefix.push_back({});
            }
            return prefix;
        }
        const IntegerColumn sub(m_alloc, ref); // Throws
        for (auto it = sub.cbegin(); it != sub.cend(); ++it) {
            unpack_prefix(*it);
        }
    }
    else {
        // a single value prefix has been shifted one to the right
        // to allow it to be tagged without losing the high bit
        unpack_prefix(rot_payload.get_as_int() << 1);
    }
    // there may have been some extra chunks read in the last value
    if (prefix.size() > num_chunks) {
        prefix.erase(prefix.begin() + num_chunks, prefix.end());
    }
    REALM_ASSERT_3(prefix.size(), ==, num_chunks);
    return prefix;
}

template <size_t ChunkWidth>
void IndexNode::set_prefix(const typename IndexKey<ChunkWidth>::Prefix& prefix)
{
    set(c_ndx_of_prefix_size, RefOrTagged::make_tagged(prefix.size()));
    RefOrTagged rot_payload = get_as_ref_or_tagged(c_ndx_of_prefix_payload);
    if (rot_payload.is_ref() && rot_payload.get_as_ref() != 0) {
        IntegerColumn sub(m_alloc, rot_payload.get_as_ref()); // Throws
        sub.destroy();
        // TODO: maybe optimize this by looking for a common prefix?
    }
    if (!prefix.size()) {
        return;
    }

    std::vector<int64_t> packed_prefix;
    for (size_t i = 0; i < prefix.size(); ++i) {
        const size_t chunk_for_value = i % IndexKey<ChunkWidth>::c_key_chunks_per_prefix;
        if (chunk_for_value == 0) {
            packed_prefix.push_back(0);
        }
        const size_t lshift = 64 - ((1 + chunk_for_value) * ChunkWidth);
        packed_prefix.back() = packed_prefix.back() | (uint64_t(prefix[i].to_ullong()) << lshift);
    }

    if (packed_prefix.size() == 1) {
        // shift 1 right so it doesn't overflow, we know there is space for this
        // because the calculation of c_key_chunks_per_prefix accounts for it
        set(c_ndx_of_prefix_payload, RefOrTagged::make_tagged(uint64_t(packed_prefix[0]) >> 1));
    }
    else if (packed_prefix.size() > 1) {
        IntegerColumn sub(m_alloc);
        sub.set_parent(this, c_ndx_of_prefix_payload);
        sub.create();
        for (uint64_t val : packed_prefix) {
            sub.add(val);
        }
    }
}

template <size_t ChunkWidth>
void IndexNode::advance_key_to_existing_prefix(IndexKey<ChunkWidth>& key)
{
    if (!key.get()) {
        return;
    }
    size_t prefix_size = 0;
    RefOrTagged rot_prefix_size = get_as_ref_or_tagged(c_ndx_of_prefix_size);
    if (rot_prefix_size.is_tagged()) {
        prefix_size = rot_prefix_size.get_as_int();
    }
    else {
        REALM_ASSERT_3(rot_prefix_size.get_as_ref(), ==, 0);
        //        if (prefixes.size() == 1) {
        //            Array::set
        //        }
    }

    RefOrTagged rot_prefix_payload = get_as_ref_or_tagged(c_ndx_of_prefix_payload);
    if (rot_prefix_payload.is_tagged()) {
        REALM_ASSERT_3(prefix_size, <, IndexKey<ChunkWidth>::c_key_chunks_per_prefix);
    }
    else {
    }
}

template <size_t ChunkWidth>
void IndexNode::do_prefix_insert(IndexKey<ChunkWidth>& key)
{
    REALM_ASSERT_DEBUG(key.get());
    if (is_empty()) {
        auto prefix = key.advance_chunks(); // get full prefix to end
        set_prefix<ChunkWidth>(prefix);
        return;
    }
    auto existing_prefix = get_prefix<ChunkWidth>();
    if (existing_prefix.size() == 0) {
        // not empty and no prefix; no common prefix
        return;
    }
    size_t orig_offset = key.get_offset();
    // FIXME: advance one by one
    auto possible_shared_prefix = key.advance_chunks(existing_prefix.size());

    // find common prefix
    size_t ndx_of_last_shared_chunk = 0;
    while (ndx_of_last_shared_chunk < possible_shared_prefix.size() &&
           ndx_of_last_shared_chunk < existing_prefix.size()) {
        if (existing_prefix[ndx_of_last_shared_chunk] != possible_shared_prefix[ndx_of_last_shared_chunk]) {
            break;
        }
        ++ndx_of_last_shared_chunk;
    }
    if (ndx_of_last_shared_chunk != existing_prefix.size()) {
        // split the prefix
        typename IndexKey<ChunkWidth>::Prefix shared_prefix;
        typename IndexKey<ChunkWidth>::Prefix prefix_to_move;
        shared_prefix.insert(shared_prefix.begin(), possible_shared_prefix.begin(),
                             possible_shared_prefix.begin() + ndx_of_last_shared_chunk);
        prefix_to_move.insert(prefix_to_move.begin(), existing_prefix.begin() + ndx_of_last_shared_chunk,
                              existing_prefix.end());

        const Array::Type type = Array::type_HasRefs;
        std::unique_ptr<IndexNode> split_node = std::make_unique<IndexNode>(m_alloc); // Throws
        // Mark that this is part of index
        // (as opposed to columns under leaves)
        constexpr bool set_context_flag = true;
        split_node->Array::create(type, set_context_flag); // Throws
        Array::move(*split_node, 0);
        for (size_t i = 0; i < c_num_metadata_entries; ++i) {
            Array::add(0);
        }
        REALM_ASSERT(prefix_to_move.size() != 0);
        Array::set(c_ndx_of_null, split_node->get(c_ndx_of_null));
        split_node->set(c_ndx_of_null, 0);
        uint64_t population_split = prefix_to_move[0].to_ullong();
        prefix_to_move.erase(prefix_to_move.begin());
        split_node->set_prefix<ChunkWidth>(prefix_to_move);
        set_prefix<ChunkWidth>(shared_prefix);
        do_insert_to_population(population_split);
        add(split_node->get_ref());
        key.set_offset(orig_offset + ndx_of_last_shared_chunk);
    }
    // otherwise the entire prefix is shared
}

template <size_t ChunkWidth>
IndexNode::InsertResult IndexNode::insert_to_population(IndexKey<ChunkWidth>& key)
{
    auto optional_value = key.get();
    if (!optional_value) {
        InsertResult ret;
        ret.did_exist = Array::get(c_ndx_of_null) == 0;
        ret.real_index = c_ndx_of_null;
        return ret;
    }

    do_prefix_insert(key);
    optional_value = key.get(); // do_prefix_insert may have advanced the key
    REALM_ASSERT(optional_value);
    return do_insert_to_population(*optional_value);
}

IndexNode::InsertResult IndexNode::do_insert_to_population(uint64_t value)
{
    InsertResult ret = {true, realm::npos};
    // can only store 63 entries per population slot because population is a tagged value
    REALM_ASSERT_EX(value <= (63 + 63), value);
    if (value < 63) {
        uint64_t population = get_population_0();
        if ((population & (uint64_t(1) << value)) == 0) {
            // no entry for this yet, add one
            population = population | (uint64_t(1) << value);
            set_population_0(population);
            ret.did_exist = false;
        }
        ret.real_index = c_num_metadata_entries + fast_popcount64(population << (63 - value)) - 1;
    }
    else {
        value -= 63;
        uint64_t population_0 = get_population_0();
        uint64_t population_1 = get_population_1();

        if ((population_1 & (uint64_t(1) << value)) == 0) {
            // no entry for this yet, add one
            population_1 = population_1 | (uint64_t(1) << value);
            set_population_1(population_1);
            ret.did_exist = false;
        }
        ret.real_index = c_num_metadata_entries + fast_popcount64(population_1 << (63 - value)) +
                         fast_popcount64(population_0) - 1;
    }
    return ret;
}

template <size_t ChunkWidth>
std::optional<size_t> IndexNode::index_of(const IndexKey<ChunkWidth>& key) const
{
    auto optional_value = key.get();
    if (!optional_value) {
        return Array::get(c_ndx_of_null) ? std::make_optional(c_ndx_of_null) : std::nullopt;
    }
    size_t value = *optional_value;
    if (value < 63) {
        uint64_t population = get_population_0();
        if ((population & (uint64_t(1) << value)) == 0) {
            return std::nullopt;
        }
        return c_num_metadata_entries + fast_popcount64(population << (63 - value)) - 1;
    }
    value -= 63;

    uint64_t population_0 = get_population_0();
    uint64_t population_1 = get_population_1();

    if ((population_1 & (uint64_t(1) << value)) == 0) {
        return std::nullopt;
    }
    return c_num_metadata_entries + fast_popcount64(population_1 << (63 - value)) + fast_popcount64(population_0) - 1;
}

bool IndexNode::has_duplicate_values() const
{
    RefOrTagged rot_null = get_as_ref_or_tagged(c_ndx_of_null);
    if (rot_null.is_ref()) {
        if (ref_type nulls_ref = rot_null.get_as_ref()) {
            return true; // list of nulls exists
        }
    }
    std::vector<ref_type> nodes_to_check = {this->get_ref()};
    while (!nodes_to_check.empty()) {
        IndexNode node(m_alloc);
        node.init_from_ref(nodes_to_check.back());
        nodes_to_check.pop_back();
        const size_t size = node.size();
        for (size_t i = c_ndx_of_null; i < size; ++i) {
            RefOrTagged rot = node.get_as_ref_or_tagged(i);
            if (rot.is_ref() && rot.get_as_ref() != 0) {
                ref_type ref = rot.get_as_ref();
                char* header = m_alloc.translate(ref);
                // ref to sorted list
                if (!Array::get_context_flag_from_header(header)) {
                    return true; // column of duplicates
                }
                // otherwise this is a nested IndexNode that needs checking
                nodes_to_check.push_back(rot.get_as_ref());
            }
        }
    }
    return false;
}

bool IndexNode::is_empty() const
{
    return get_population_0() == 0 && get_population_1() == 0 && get(c_ndx_of_null) == 0;
}

template <size_t ChunkWidth>
void IndexNode::verify() const
{
    size_t actual_size = size();
    REALM_ASSERT(actual_size >= c_num_metadata_entries);
    uint64_t population_0 = get_population_0();
    uint64_t population_1 = get_population_1();
    size_t num_entries = size_t(fast_popcount64(population_0) + fast_popcount64(population_1));
    REALM_ASSERT_EX(num_entries + c_num_metadata_entries == actual_size, num_entries, actual_size,
                    c_num_metadata_entries);
    //    auto prefix = get_prefix<ChunkWidth>();
}

template <size_t ChunkWidth>
void IndexNode::print() const
{
    struct NodeInfo {
        ref_type ref;
        size_t depth;
    };
    std::vector<NodeInfo> sub_nodes = {{this->get_ref(), 0}};

    while (!sub_nodes.empty()) {
        std::unique_ptr<IndexNode> cur_node = std::make_unique<IndexNode>(m_alloc);
        cur_node->init_from_ref(sub_nodes.begin()->ref);
        const size_t cur_depth = sub_nodes.begin()->depth;
        sub_nodes.erase(sub_nodes.begin());

        size_t array_size = cur_node->size();
        uint64_t pop_0 = cur_node->get_population_0();
        uint64_t pop_1 = cur_node->get_population_1();
        int64_t nulls = cur_node->get(c_ndx_of_null);
        auto prefix = cur_node->get_prefix<ChunkWidth>();
        std::string prefix_str = "";
        if (prefix.size()) {
            prefix_str = util::format("%1 chunk prefix: '", prefix.size());
            for (auto val : prefix) {
                prefix_str += util::format("%1, ", val.to_ullong());
            }
            prefix_str.replace(prefix_str.size() - 2, 2, "'");
        }
        std::string null_str = "";
        if (nulls) {
            if (nulls & 1) {
                null_str = util::format("null %1, ", nulls >> 1);
            }
            else {
                null_str = "list of nulls, ";
            }
        }
        util::format(std::cout, "IndexNode[%1] depth %2, size %3, population [%4, %5], %6%7: {", cur_node->get_ref(),
                     cur_depth, array_size, pop_0, pop_1, null_str, prefix_str);
        for (size_t i = c_num_metadata_entries; i < array_size; ++i) {
            if (i > c_num_metadata_entries) {
                std::cout << ", ";
            }
            auto rot = cur_node->get_as_ref_or_tagged(i);
            if (rot.is_ref()) {
                ref_type ref = rot.get_as_ref();
                if (ref == 0) {
                    std::cout << "NULL";
                    continue;
                }
                char* header = m_alloc.translate(ref);
                // ref to sorted list
                if (!Array::get_context_flag_from_header(header)) {
                    const IntegerColumn sub(m_alloc, rot.get_as_ref()); // Throws
                    std::cout << "list{";
                    for (size_t j = 0; j < sub.size(); ++j) {
                        if (j != 0) {
                            std::cout << ", ";
                        }
                        std::cout << "ObjKey(" << sub.get(j) << ")";
                    }
                    std::cout << "}";
                }
                else {
                    std::cout << "ref[" << rot.get_as_ref() << "]";
                    sub_nodes.push_back({rot.get_as_ref(), cur_depth + 1});
                }
            }
            else {
                std::cout << "ObjKey(" << rot.get_as_int() << ")";
            }
        }
        std::cout << "}\n";
    }
}

template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::insert(ObjKey value, const Mixed& key)
{
    insert(value, IndexKey<ChunkWidth>(key));
}

template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::insert(ObjKey key, IndexKey<ChunkWidth> value)
{
    m_array->update_from_parent();
    m_array->insert(key, value);
}

template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::erase(ObjKey key)
{
    Mixed value = m_target_column.get_value(key);
    erase(key, value);
}

template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::erase(ObjKey key, const Mixed& value)
{
    IndexKey<ChunkWidth> index_value(value);
    m_array->update_from_parent();
    m_array->erase(key, index_value);
}

template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::set(ObjKey key, const Mixed& new_value)
{
    Mixed old_value = m_target_column.get_value(key);
    if (REALM_LIKELY(new_value != old_value)) {
        // We must erase this row first because erase uses find_first which
        // might find the duplicate if we insert before erasing.
        erase(key); // Throws
        insert(key, new_value);
    }
}

template <size_t ChunkWidth>
ObjKey RadixTree<ChunkWidth>::find_first(const Mixed& val) const
{
    m_array->update_from_parent();
    return m_array->find_first(IndexKey<ChunkWidth>(val)).get_key();
}

template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::find_all(std::vector<ObjKey>& result, Mixed value, bool case_insensitive) const
{
    REALM_ASSERT_RELEASE(!case_insensitive);
    m_array->update_from_parent();
    m_array->find_all(result, IndexKey<ChunkWidth>(value));
}

template <size_t ChunkWidth>
FindRes RadixTree<ChunkWidth>::find_all_no_copy(Mixed value, InternalFindResult& result) const
{
    m_array->update_from_parent();
    return m_array->find_all_no_copy(IndexKey<ChunkWidth>(value), result);
}

template <size_t ChunkWidth>
size_t RadixTree<ChunkWidth>::count(const Mixed& val) const
{
    m_array->update_from_parent();
    auto it = m_array->find_first(IndexKey<ChunkWidth>(val));
    return it.num_matches();
}

template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::clear()
{
    m_array->update_from_parent();
    m_array->clear();
}

template <size_t ChunkWidth>
bool RadixTree<ChunkWidth>::has_duplicate_values() const noexcept
{
    m_array->update_from_parent();
    return m_array->has_duplicate_values();
}

template <size_t ChunkWidth>
bool RadixTree<ChunkWidth>::is_empty() const
{
    m_array->update_from_parent();
    return m_array->is_empty();
}

template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::insert_bulk(const ArrayUnsigned* keys, uint64_t key_offset, size_t num_values,
                                        ArrayPayload& values)
{
    if (keys) {
        for (size_t i = 0; i < num_values; ++i) {
            ObjKey key(keys->get(i) + key_offset);
            insert(key, values.get_any(i));
        }
    }
    else {
        for (size_t i = 0; i < num_values; ++i) {
            ObjKey key(i + key_offset);
            insert(key, values.get_any(i));
        }
    }
}

template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::verify() const
{
    m_array->update_from_parent();
    m_array->verify<ChunkWidth>();
}

#ifdef REALM_DEBUG
template <size_t ChunkWidth>
void RadixTree<ChunkWidth>::print() const
{
    m_array->update_from_parent();
    m_array->print<ChunkWidth>();
}
#endif // REALM_DEBUG

template class RadixTree<6>;

} // namespace realm
