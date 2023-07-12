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

#ifndef REALM_RADIX_TREE_HPP
#define REALM_RADIX_TREE_HPP

#include <realm/array.hpp>
#include <realm/cluster_tree.hpp>
#include <realm/search_index.hpp>

namespace realm {

class IndexNode;

struct IndexIterator {
    IndexIterator& operator++();
    IndexIterator next() const;
    size_t num_matches() const;

    ObjKey get_key() const
    {
        return m_key;
    }
    operator bool() const
    {
        return bool(m_key);
    }

private:
    IndexNode* last_node() const
    {
        if (m_accessor_chain.size()) {
            return m_accessor_chain.back().get();
        }
        return nullptr;
    }
    std::vector<size_t> m_positions;
    std::optional<size_t> m_list_position;
    ObjKey m_key;
    std::vector<std::unique_ptr<IndexNode>> m_accessor_chain;
    friend class IndexNode;
};

template <size_t ChunkWidth>
class IndexKey {
public:
    IndexKey(Mixed m)
        : m_mixed(m)
    {
    }
    std::optional<size_t> get() const
    {
        static_assert(ChunkWidth < 64, "chunks must be less than 64 bits");
        constexpr uint64_t int_mask = (~uint64_t(0) >> (64 - ChunkWidth)) << (64 - ChunkWidth);
        if (m_mixed.is_null()) {
            return {};
        }
        size_t ret = 0;
        if (m_mixed.is_type(type_Int)) {
            size_t rshift = (1 + m_offset) * ChunkWidth;
            rshift = rshift < 64 ? 64 - rshift : 0;
            ret = (uint64_t(m_mixed.get<Int>()) & (int_mask >> (m_offset * ChunkWidth))) >> rshift;
            REALM_ASSERT_3(ret, <, (1 << ChunkWidth));
            return ret;
        }
        //        if (m_mixed.is_type(type_String)) {
        //            REALM_ASSERT_EX(ChunkWidth == 8, ChunkWidth); // FIXME: other sizes for strings
        //            return m_mixed.get<StringData>()[m_offset];
        //        }
        REALM_UNREACHABLE(); // FIXME: implement if needed
    }
    std::optional<size_t> get_next()
    {
        ++m_offset;
        return get();
    }
    bool is_last() const
    {
        if (m_mixed.is_null()) {
            return true;
        }
        if (m_mixed.is_type(type_Int)) {
            return (m_offset * ChunkWidth) + ChunkWidth >= 64;
        }
        REALM_UNREACHABLE(); // FIXME: other types
    }
    const Mixed& get_mixed() const
    {
        return m_mixed;
    }

private:
    size_t m_offset = 0;
    Mixed m_mixed;
};

/// Each RadixTree node contains an array of this type
class IndexNode : public Array {
public:
    IndexNode(Allocator& allocator)
        : Array(allocator)
    {
    }

    static std::unique_ptr<IndexNode> create(Allocator& alloc);

    template <size_t ChunkWidth>
    void insert(ObjKey value, IndexKey<ChunkWidth> key);
    template <size_t ChunkWidth>
    void erase(ObjKey value, IndexKey<ChunkWidth> key);
    template <size_t ChunkWidth>
    IndexIterator find_first(IndexKey<ChunkWidth> key) const;
    template <size_t ChunkWidth>
    void find_all(std::vector<ObjKey>& results, IndexKey<ChunkWidth> key) const;
    template <size_t ChunkWidth>
    FindRes find_all_no_copy(IndexKey<ChunkWidth> value, InternalFindResult& result) const;
    void clear();
    bool has_duplicate_values() const;
    bool is_empty() const;

    void print() const;
    void verify() const;

private:
    template <size_t ChunkWidth>
    std::unique_ptr<IndexNode> do_add_direct(ObjKey value, size_t ndx, IndexKey<ChunkWidth>& key);
    constexpr static size_t c_ndx_of_population_0 = 0;
    constexpr static size_t c_ndx_of_population_1 = 1;
    constexpr static size_t c_ndx_of_prefix = 2;
    constexpr static size_t c_ndx_of_null = 3; // adjacent to data so that iteration works
    constexpr static size_t c_num_metadata_entries = 4;

    uint64_t get_population_0() const;
    uint64_t get_population_1() const;
    void set_population_0(uint64_t pop);
    void set_population_1(uint64_t pop);

    struct InsertResult {
        bool did_exist;
        size_t real_index;
    };
    template <size_t ChunkWidth>
    InsertResult insert_to_population(const IndexKey<ChunkWidth>& key);
    template <size_t ChunkWidth>
    std::optional<size_t> index_of(const IndexKey<ChunkWidth>& key) const;
    bool do_remove(size_t index_raw);
};

template <size_t ChunkWidth>
class RadixTree : public SearchIndex {
public:
    RadixTree(const ClusterColumn&, Allocator&);
    RadixTree(ref_type, ArrayParent*, size_t, const ClusterColumn& target_column, Allocator&);
    ~RadixTree() = default;

    // SearchIndex overrides:
    void insert(ObjKey value, const Mixed& key) final;
    void set(ObjKey value, const Mixed& key) final;
    ObjKey find_first(const Mixed&) const final;
    void find_all(std::vector<ObjKey>& result, Mixed value, bool case_insensitive = false) const final;
    FindRes find_all_no_copy(Mixed value, InternalFindResult& result) const final;
    size_t count(const Mixed&) const final;
    void erase(ObjKey) final;
    void clear() final;
    bool has_duplicate_values() const noexcept final;
    bool is_empty() const final;
    void insert_bulk(const ArrayUnsigned* keys, uint64_t key_offset, size_t num_values, ArrayPayload& values) final;
    void verify() const final;

#ifdef REALM_DEBUG
    void print() const final;
#endif // REALM_DEBUG

    // RadixTree specials
    void insert(ObjKey value, IndexKey<ChunkWidth> key);
    IndexIterator find(IndexKey<ChunkWidth> key);

private:
    void erase(ObjKey key, const Mixed& new_value);

    RadixTree(const ClusterColumn& target_column, std::unique_ptr<IndexNode> root)
        : SearchIndex(target_column, root.get())
        , m_array(std::move(root))
    {
    }
    std::unique_ptr<IndexNode> m_array;
};

// Implementation:
template <size_t ChunkWidth>
RadixTree<ChunkWidth>::RadixTree(const ClusterColumn& target_column, Allocator& alloc)
    : RadixTree(target_column, IndexNode::create(alloc))
{
}

template <size_t ChunkWidth>
inline RadixTree<ChunkWidth>::RadixTree(ref_type ref, ArrayParent* parent, size_t ndx_in_parent,
                                        const ClusterColumn& target_column, Allocator& alloc)
    : RadixTree(target_column, std::make_unique<IndexNode>(alloc))
{
    REALM_ASSERT_EX(Array::get_context_flag_from_header(alloc.translate(ref)), ref, size_t(alloc.translate(ref)));
    m_array->init_from_ref(ref);
    m_array->set_parent(parent, ndx_in_parent);
}

using IntegerIndex = RadixTree<6>;

} // namespace realm

#endif // REALM_RADIX_TREE_HPP
