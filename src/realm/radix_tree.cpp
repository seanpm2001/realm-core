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

#include "radix_tree_templates.cpp"


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


template class RadixTree<6>;

} // namespace realm
