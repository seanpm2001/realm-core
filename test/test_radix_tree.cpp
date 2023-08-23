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
#include "test.hpp"
#include "util/misc.hpp"
#include "util/random.hpp"

using namespace realm;
using namespace util;
using namespace realm;
using namespace realm::util;
using namespace realm::test_util;
using unit_test::TestContext;

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
    int_index->print();

    for (auto val : values) {
        const ObjKey key = int_index->find_first(val);
        CHECK(key);
    }
    CHECK_EQUAL(int_index->count(4), 2);
    CHECK_EQUAL(int_index->count(5), 3);
    while (table.size()) {
        table.remove_object(table.begin());
    }
    int_index->print();
}

#endif // TEST_RADIX_TREE
