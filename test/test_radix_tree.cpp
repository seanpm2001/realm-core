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

template <size_t ChunkSize>
class TableForIndexWidth : public Table {
    std::unique_ptr<SearchIndex> make_index(ColKey col_key, const ClusterColumn& cluster) override
    {
        if (col_key.get_type() == col_type_Int) {
            return std::make_unique<RadixTree<ChunkSize>>(cluster, get_alloc()); // Throws
        }
        return std::make_unique<StringIndex>(cluster, get_alloc()); // Throws
    }

    std::unique_ptr<SearchIndex> make_index(ColKey col_key, ref_type ref, Array& parent, size_t col_ndx,
                                            const ClusterColumn& virtual_col) override
    {
        if (col_key.get_type() == col_type_Int) {
            return std::make_unique<IntegerIndex>(ref, &parent, col_ndx, virtual_col, get_alloc());
        }
        return std::make_unique<StringIndex>(ref, &parent, col_ndx, virtual_col, get_alloc());
    }
};

namespace realm {
template class RadixTree<7>;
} // namespace realm

ONLY_TYPES(IndexNode, /*ChunkOf<4>, ChunkOf<5>,*/ ChunkOf<6>, ChunkOf<7> /* ChunkOf<8>, ChunkOf<9>, ChunkOf<10>*/)
{
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
    std::vector<ObjKey> keys;
    constexpr size_t ChunkWidth = TEST_TYPE::value;

    TableForIndexWidth<ChunkWidth> table;
    ColKey col = table.add_column(type_Int, "values");
    for (size_t i = 0; i < values.size(); ++i) {
        auto obj = table.create_object().set(col, values[i]);
        keys.push_back(obj.get_key());
    }
    //    const ObjKey key_of_null{999};
    //    keys.push_back(key_of_null);
    //    tree.insert(key_of_null, Mixed{});

    for (size_t i = 0; i < values.size(); ++i) {
        size_t expected_count = 1;
        int64_t val = values[i];
        if (val == dup_positive || val == dup_negative) {
            expected_count = 4;
        }
        size_t count = table.count_int(col, val);
        CHECK_EQUAL(count, expected_count);
        if (expected_count == 1) {
            ObjKey found_key = table.find_first_int(col, val);
            ObjKey expected_key = keys[i];
            CHECK_EQUAL(found_key, expected_key);
        }
    }
    //    CHECK_EQUAL(tree.count(Mixed{}), 1);
    //    CHECK_EQUAL(tree.find_first(Mixed{}), key_of_null);
    //    CHECK(!tree.is_empty());

    for (auto key : keys) {
        table.remove_object(key);
    }
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
