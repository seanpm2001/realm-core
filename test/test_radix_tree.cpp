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

#include "testsettings.hpp"
#ifdef TEST_RADIX_TREE

#include <realm.hpp>
#include <realm/index_string.hpp>
#include <realm/query_expression.hpp>
#include <realm/tokenizer.hpp>
#include <realm/util/to_string.hpp>
#include <set>
#include <type_traits>
#include "test.hpp"
#include "util/misc.hpp"
#include "util/random.hpp"


// We are interested in testing various sizes of the tree
// but we don't want the core shared library to pay the size
// cost of storing these symbols when none of the SDKs will use
// them. To get around this using explicit template instantiation,
// we include the radix_tree_templates.cpp file here which _only_
// has templated code and instantiate it below.
#include <realm/radix_tree_templates.cpp>
namespace realm {
template class RadixTree<7>;
template class RadixTree<8>;
template class RadixTree<9>;
template class RadixTree<10>;
template class RadixTree<16>;
} // namespace realm


using namespace realm;
using namespace util;
using namespace realm;
using namespace realm::util;
using namespace realm::test_util;
using unit_test::TestContext;


template <size_t Chunk>
using ChunkOf = typename std::integral_constant<size_t, Chunk>;

TEST_TYPES(IndexKey_Get, ChunkOf<4>, ChunkOf<5>, ChunkOf<6>, ChunkOf<7>, ChunkOf<8>, ChunkOf<9>, ChunkOf<10>)
{
    constexpr size_t ChunkWidth = TEST_TYPE::value;

    CHECK(!IndexKey<ChunkWidth>(Mixed{}).get());

    size_t max = uint64_t(1) << ChunkWidth;
    for (size_t i = 0; i < max; ++i) {
        uint64_t shifted_value = uint64_t(i) << (64 - ChunkWidth);
        IndexKey<ChunkWidth> key{int64_t(shifted_value)};
        auto val = key.get();
        CHECK(val);
        CHECK_EQUAL(i, *val);
        CHECK(!key.is_last());

        const size_t num_chunks_per_int = std::ceil(double(64) / double(ChunkWidth));

        size_t chunk_count = 1;
        while (!key.is_last()) {
            key.next();
            val = key.get();
            CHECK(val);
            CHECK_EQUAL(*val, 0);
            ++chunk_count;
        }
        CHECK_EQUAL(chunk_count, num_chunks_per_int);
    }
}

TEST_TYPES(IndexNode, ChunkOf<4>, ChunkOf<5>, ChunkOf<6>, ChunkOf<7>, ChunkOf<8>, ChunkOf<9>, ChunkOf<10>)
{
    constexpr size_t ChunkWidth = TEST_TYPE::value;

    Table::IndexMaker hook = [](ColKey col_key, const ClusterColumn& cluster, Allocator& alloc, ref_type ref,
                                Array* parent, size_t col_ndx) -> std::unique_ptr<SearchIndex> {
        if (parent) {
            if (col_key.get_type() == col_type_Int || col_key.get_type() == col_type_Timestamp) {
                return std::make_unique<RadixTree<ChunkWidth>>(ref, parent, col_ndx, cluster, alloc);
            }
            return std::make_unique<StringIndex>(ref, parent, col_ndx, cluster, alloc);
        }
        if (col_key.get_type() == col_type_Int || col_key.get_type() == col_type_Timestamp) {
            return std::make_unique<RadixTree<ChunkWidth>>(cluster, alloc); // Throws
        }
        return std::make_unique<StringIndex>(cluster, alloc); // Throws
    };

    int64_t dup_positive = 8;
    int64_t dup_negative = -77;
    std::vector<int64_t> values = {
        0,
        1,
        2,
        3,
        4,
        5,
        -1,
        -2,
        -3,
        -4,
        -5,
        dup_positive,
        dup_positive,
        dup_positive,
        dup_positive,
        dup_negative,
        dup_negative,
        dup_negative,
        dup_negative,
        0xF00000000000000,
        0xFF0000000000000,
        0xFFF000000000000,
        0xFFFFF0000000000,
        0xFFFFFFFFFFFFFFF,
        0xEEE000000000000,
        0xEFF000000000000,
    };
    auto timestamp_from_int = [](int64_t val) -> Timestamp {
        int32_t ns = uint64_t(val) >> 32;
        if (val < 0 && ns > 0) {
            ns *= -1;
        }
        return Timestamp{val, ns};
    };
    std::vector<ObjKey> keys;

    Table table(Allocator::get_default(), std::move(hook));
    constexpr bool nullable = true;
    ColKey col_int = table.add_column(type_Int, "values_int", nullable);
    ColKey col_timestamp = table.add_column(type_Timestamp, "values_timestamp", nullable);
    table.add_search_index(col_int);
    table.add_search_index(col_timestamp);

    for (auto val : values) {
        auto obj = table.create_object().set(col_int, val).set(col_timestamp, timestamp_from_int(val));
        keys.push_back(obj.get_key());
    }
    Obj null_val_obj = table.create_object().set_null(col_int).set_null(col_timestamp);
    keys.push_back(null_val_obj.get_key());

    SearchIndex* ndx_int = table.get_search_index(col_int);
    CHECK(ndx_int);
    SearchIndex* ndx_timestamp = table.get_search_index(col_timestamp);
    CHECK(ndx_timestamp);

    for (size_t i = 0; i < values.size(); ++i) {
        size_t expected_count = 1;
        int64_t val = values[i];
        Timestamp val_timestamp = timestamp_from_int(val);
        if (val == dup_positive || val == dup_negative) {
            expected_count = 4;
        }
        CHECK_EQUAL(expected_count, ndx_int->count(val));
        CHECK_EQUAL(expected_count, ndx_timestamp->count(val_timestamp));
        if (expected_count == 1) {
            ObjKey expected_key = keys[i];
            CHECK_EQUAL(expected_key, ndx_int->find_first(val));
            CHECK_EQUAL(expected_key, ndx_timestamp->find_first(val_timestamp));
        }
    }
    CHECK_EQUAL(null_val_obj.get_key(), ndx_int->find_first(Mixed{}));
    CHECK_EQUAL(null_val_obj.get_key(), ndx_timestamp->find_first(Mixed{}));
    CHECK_EQUAL(ndx_int->count(Mixed{}), 1);
    CHECK_EQUAL(ndx_timestamp->count(Mixed{}), 1);
    CHECK(ndx_int->has_duplicate_values());
    CHECK(ndx_timestamp->has_duplicate_values());
    CHECK_NOT(ndx_int->is_empty());
    CHECK_NOT(ndx_timestamp->is_empty());

    for (auto key : keys) {
        table.remove_object(key);
    }
    CHECK_EQUAL(ndx_int->count(Mixed{}), 0);
    CHECK_EQUAL(ndx_timestamp->count(Mixed{}), 0);
    CHECK_NOT(ndx_int->has_duplicate_values());
    CHECK_NOT(ndx_timestamp->has_duplicate_values());
    CHECK(ndx_int->is_empty());
    CHECK(ndx_timestamp->is_empty());
    CHECK_EQUAL(ndx_int->find_first(Mixed{}), ObjKey{});
    CHECK_EQUAL(ndx_timestamp->find_first(Mixed{}), ObjKey{});

    CHECK(table.is_empty());
    table.clear();
    CHECK(table.is_empty());
}


TEST(RadixTree_BuildIndex)
{
    std::vector<Mixed> values = {0, 1, 2, 3, 4, 4, 5, 5, 5, Mixed{}, -1};
    Table table;
    ColKey col_pk = table.add_column(type_ObjectId, "pk");
    table.set_primary_key_column(col_pk);
    const bool nullable = true;
    ColKey col_key = table.add_column(type_Int, "values", nullable);

    for (auto val : values) {
        table.create_object_with_primary_key(ObjectId::gen()).set_any(col_key, val);
    }

    // Create a new index on column
    table.add_search_index(col_key);
    IntegerIndex* int_index = table.get_int_index(col_key);
    CHECK(int_index);

    for (auto val : values) {
        const ObjKey key = int_index->find_first(val);
        CHECK(key);
    }
    CHECK_EQUAL(int_index->count(4), 2);
    CHECK_EQUAL(int_index->count(5), 3);
    while (table.size()) {
        table.remove_object(table.begin());
    }
}

#endif // TEST_RADIX_TREE
