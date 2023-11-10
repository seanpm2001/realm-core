/*************************************************************************
 *
 * Copyright 2021 Realm Inc.
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

#include "realm/parser/query_ast.hpp"
#include "realm/parser/keypath_mapping.hpp"
#include "realm/parser/query_parser.hpp"
#include "realm/sort_descriptor.hpp"
#include "realm/decimal128.hpp"
#include "realm/uuid.hpp"
#include "realm/util/base64.hpp"
#include "realm/util/overload.hpp"
#include "realm/object-store/class.hpp"

#define YY_NO_UNISTD_H 1
#define YY_NO_INPUT 1
#include "realm/parser/generated/query_flex.hpp"

#include <external/mpark/variant.hpp>
#include <stdexcept>

using namespace realm;
using namespace std::string_literals;

// Whether to generate parser debug traces.
static bool trace_parsing = false;
// Whether to generate scanner debug traces.
static bool trace_scanning = false;

namespace {

const char* agg_op_type_to_str(query_parser::AggrNode::Type type)
{
    switch (type) {
        case realm::query_parser::AggrNode::MAX:
            return ".@max";
        case realm::query_parser::AggrNode::MIN:
            return ".@min";
        case realm::query_parser::AggrNode::SUM:
            return ".@sum";
        case realm::query_parser::AggrNode::AVG:
            return ".@avg";
    }
    return "";
}

const char* expression_cmp_type_to_str(util::Optional<ExpressionComparisonType> type)
{
    if (type) {
        switch (*type) {
            case ExpressionComparisonType::Any:
                return "ANY";
            case ExpressionComparisonType::All:
                return "ALL";
            case ExpressionComparisonType::None:
                return "NONE";
        }
    }
    return "";
}

std::string print_pretty_objlink(const ObjLink& link, const Group* g)
{
    REALM_ASSERT(g);
    if (link.is_null()) {
        return "NULL";
    }
    try {
        auto table = g->get_table(link.get_table_key());
        if (!table) {
            return "link to an invalid table";
        }
        auto obj = table->get_object(link.get_obj_key());
        Mixed pk = obj.get_primary_key();
        return util::format("'%1' with primary key '%2'", table->get_class_name(), util::serializer::print_value(pk));
    }
    catch (...) {
        return "invalid link";
    }
}

bool is_length_suffix(const std::string& s)
{
    return s.size() == 6 && (s[0] == 'l' || s[0] == 'L') && (s[1] == 'e' || s[1] == 'E') &&
           (s[2] == 'n' || s[2] == 'N') && (s[3] == 'g' || s[3] == 'G') && (s[4] == 't' || s[4] == 'T') &&
           (s[5] == 'h' || s[5] == 'H');
}

template <typename T>
inline bool try_parse_specials(std::string str, T& ret)
{
    if constexpr (realm::is_any<T, float, double>::value || std::numeric_limits<T>::is_iec559) {
        std::transform(str.begin(), str.end(), str.begin(), toLowerAscii);
        if (std::numeric_limits<T>::has_quiet_NaN && (str == "nan" || str == "+nan")) {
            ret = std::numeric_limits<T>::quiet_NaN();
            return true;
        }
        else if (std::numeric_limits<T>::has_quiet_NaN && (str == "-nan")) {
            ret = -std::numeric_limits<T>::quiet_NaN();
            return true;
        }
        else if (std::numeric_limits<T>::has_infinity &&
                 (str == "+infinity" || str == "infinity" || str == "+inf" || str == "inf")) {
            ret = std::numeric_limits<T>::infinity();
            return true;
        }
        else if (std::numeric_limits<T>::has_infinity && (str == "-infinity" || str == "-inf")) {
            ret = -std::numeric_limits<T>::infinity();
            return true;
        }
    }
    return false;
}

template <typename T>
inline const char* get_type_name()
{
    return "unknown";
}
template <>
inline const char* get_type_name<int64_t>()
{
    return "number";
}
template <>
inline const char* get_type_name<float>()
{
    return "floating point number";
}
template <>
inline const char* get_type_name<double>()
{
    return "floating point number";
}

template <typename T>
inline T string_to(const std::string& s)
{
    std::istringstream iss(s);
    iss.imbue(std::locale::classic());
    T value;
    iss >> value;
    if (iss.fail()) {
        if (!try_parse_specials(s, value)) {
            throw InvalidQueryArgError(util::format("Cannot convert '%1' to a %2", s, get_type_name<T>()));
        }
    }
    return value;
}

class MixedArguments : public query_parser::Arguments {
public:
    using Arg = mpark::variant<Mixed, std::vector<Mixed>>;

    MixedArguments(const std::vector<Mixed>& args)
        : Arguments(args.size())
        , m_args([](const std::vector<Mixed>& args) -> std::vector<Arg> {
            std::vector<Arg> ret;
            ret.reserve(args.size());
            for (const Mixed& m : args) {
                ret.push_back(m);
            }
            return ret;
        }(args))
    {
    }
    MixedArguments(const std::vector<Arg>& args)
        : Arguments(args.size())
        , m_args(args)
    {
    }
    bool bool_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<bool>();
    }
    long long long_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<int64_t>();
    }
    float float_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<float>();
    }
    double double_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<double>();
    }
    StringData string_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<StringData>();
    }
    BinaryData binary_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<BinaryData>();
    }
    Timestamp timestamp_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<Timestamp>();
    }
    ObjectId objectid_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<ObjectId>();
    }
    UUID uuid_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<UUID>();
    }
    Decimal128 decimal128_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<Decimal128>();
    }
    ObjKey object_index_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<ObjKey>();
    }
    ObjLink objlink_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<ObjLink>();
    }
#if REALM_ENABLE_GEOSPATIAL
    Geospatial geospatial_for_argument(size_t n) final
    {
        return mixed_for_argument(n).get<Geospatial>();
    }
#endif
    std::vector<Mixed> list_for_argument(size_t n) final
    {
        Arguments::verify_ndx(n);
        return mpark::get<std::vector<Mixed>>(m_args[n]);
    }
    bool is_argument_null(size_t n) final
    {
        Arguments::verify_ndx(n);
        return visit(util::overload{
                         [](const Mixed& m) {
                             return m.is_null();
                         },
                         [](const std::vector<Mixed>&) {
                             return false;
                         },
                     },
                     m_args[n]);
    }
    bool is_argument_list(size_t n) final
    {
        Arguments::verify_ndx(n);
        static_assert(std::is_same_v<mpark::variant_alternative_t<1, Arg>, std::vector<Mixed>>);
        return m_args[n].index() == 1;
    }
    DataType type_for_argument(size_t n)
    {
        return mixed_for_argument(n).get_type();
    }

private:
    const Mixed& mixed_for_argument(size_t n)
    {
        Arguments::verify_ndx(n);
        if (is_argument_list(n)) {
            throw InvalidQueryArgError(
                util::format("Request for scalar argument at index %1 but a list was provided", n));
        }

        return mpark::get<Mixed>(m_args[n]);
    }

    const std::vector<Arg> m_args;
};

Timestamp get_timestamp_if_valid(int64_t seconds, int32_t nanoseconds)
{
    const bool both_non_negative = seconds >= 0 && nanoseconds >= 0;
    const bool both_non_positive = seconds <= 0 && nanoseconds <= 0;
    if (both_non_negative || both_non_positive) {
        return Timestamp(seconds, nanoseconds);
    }
    throw SyntaxError("Invalid timestamp format");
}

} // namespace

namespace realm {

namespace query_parser {

std::string_view string_for_op(CompareType op)
{
    switch (op) {
        case CompareType::EQUAL:
            return "=";
        case CompareType::NOT_EQUAL:
            return "!=";
        case CompareType::GREATER:
            return ">";
        case CompareType::LESS:
            return "<";
        case CompareType::GREATER_EQUAL:
            return ">=";
        case CompareType::LESS_EQUAL:
            return "<=";
        case CompareType::BEGINSWITH:
            return "beginswith";
        case CompareType::ENDSWITH:
            return "endswith";
        case CompareType::CONTAINS:
            return "contains";
        case CompareType::LIKE:
            return "like";
        case CompareType::IN:
            return "in";
        case CompareType::TEXT:
            return "text";
    }
    return ""; // appease MSVC warnings
}

NoArguments ParserDriver::s_default_args;
query_parser::KeyPathMapping ParserDriver::s_default_mapping;

ParserNode::~ParserNode() = default;

QueryNode::~QueryNode() = default;

Query NotNode::visit(ParserDriver* drv)
{
    Query q = drv->m_base_table->where();
    q.Not();
    q.and_query(query->visit(drv));
    return {q};
}

Query OrNode::visit(ParserDriver* drv)
{
    Query q(drv->m_base_table);
    q.group();
    for (auto it : children) {
        q.Or();
        q.and_query(it->visit(drv));
    }
    q.end_group();

    return q;
}

Query AndNode::visit(ParserDriver* drv)
{
    Query q(drv->m_base_table);
    for (auto it : children) {
        q.and_query(it->visit(drv));
    }
    return q;
}

static void verify_only_string_types(DataType type, std::string_view op_string)
{
    if (type != type_String && type != type_Binary && type != type_Mixed) {
        throw InvalidQueryError(util::format(
            "Unsupported comparison operator '%1' against type '%2', right side must be a string or binary type",
            op_string, get_data_type_name(type)));
    }
}

std::unique_ptr<Subexpr> OperationNode::visit(ParserDriver* drv, DataType type)
{
    std::unique_ptr<Subexpr> left;
    std::unique_ptr<Subexpr> right;

    const bool left_is_constant = m_left->is_constant();
    const bool right_is_constant = m_right->is_constant();
    const bool produces_multiple_values = m_left->is_list() || m_right->is_list();

    if (left_is_constant && right_is_constant && !produces_multiple_values) {
        right = m_right->visit(drv, type);
        left = m_left->visit(drv, type);
        auto v_left = left->get_mixed();
        auto v_right = right->get_mixed();
        Mixed result;
        switch (m_op) {
            case '+':
                result = v_left + v_right;
                break;
            case '-':
                result = v_left - v_right;
                break;
            case '*':
                result = v_left * v_right;
                break;
            case '/':
                result = v_left / v_right;
                break;
            default:
                break;
        }
        return std::make_unique<Value<Mixed>>(result);
    }

    if (right_is_constant) {
        // Take left first - it cannot be a constant
        left = m_left->visit(drv);

        right = m_right->visit(drv, left->get_type());
    }
    else {
        right = m_right->visit(drv);
        if (left_is_constant) {
            left = m_left->visit(drv, right->get_type());
        }
        else {
            left = m_left->visit(drv);
        }
    }
    if (!Mixed::is_numeric(left->get_type(), right->get_type())) {
        util::serializer::SerialisationState state;
        std::string op(&m_op, 1);
        throw InvalidQueryArgError(util::format("Cannot perform '%1' operation on '%2' and '%3'", op,
                                                left->description(state), right->description(state)));
    }

    switch (m_op) {
        case '+':
            return std::make_unique<Operator<Plus>>(std::move(left), std::move(right));
        case '-':
            return std::make_unique<Operator<Minus>>(std::move(left), std::move(right));
        case '*':
            return std::make_unique<Operator<Mul>>(std::move(left), std::move(right));
        case '/':
            return std::make_unique<Operator<Div>>(std::move(left), std::move(right));
        default:
            break;
    }
    return {};
}

Query EqualityNode::visit(ParserDriver* drv)
{
    auto [left, right] = drv->cmp(values);

    auto left_type = left->get_type();
    auto right_type = right->get_type();

    auto handle_typed_links = [drv](std::unique_ptr<Subexpr>& list, std::unique_ptr<Subexpr>& expr, DataType& type) {
        if (auto link_column = dynamic_cast<const Columns<Link>*>(list.get())) {
            // Change all TypedLink values to ObjKey values
            auto value = dynamic_cast<ValueBase*>(expr.get());
            auto left_dest_table_key = link_column->link_map().get_target_table()->get_key();
            auto sz = value->size();
            auto obj_keys = std::make_unique<Value<ObjKey>>();
            obj_keys->init(expr->has_multiple_values(), sz);
            obj_keys->set_comparison_type(expr->get_comparison_type());
            for (size_t i = 0; i < sz; i++) {
                auto val = value->get(i);
                // i'th entry is already NULL
                if (!val.is_null()) {
                    TableKey right_table_key;
                    ObjKey right_obj_key;
                    if (val.is_type(type_Link)) {
                        right_table_key = left_dest_table_key;
                        right_obj_key = val.get<ObjKey>();
                    }
                    else if (val.is_type(type_TypedLink)) {
                        right_table_key = val.get_link().get_table_key();
                        right_obj_key = val.get_link().get_obj_key();
                    }
                    else {
                        const char* target_type = get_data_type_name(val.get_type());
                        throw InvalidQueryError(
                            util::format("Unsupported comparison between '%1' and type '%2'",
                                         link_column->link_map().description(drv->m_serializer_state), target_type));
                    }
                    if (left_dest_table_key == right_table_key) {
                        obj_keys->set(i, right_obj_key);
                    }
                    else {
                        const Group* g = drv->m_base_table->get_parent_group();
                        throw InvalidQueryArgError(
                            util::format("The relationship '%1' which links to type '%2' cannot be compared to "
                                         "an argument of type %3",
                                         link_column->link_map().description(drv->m_serializer_state),
                                         link_column->link_map().get_target_table()->get_class_name(),
                                         print_pretty_objlink(ObjLink(right_table_key, right_obj_key), g)));
                    }
                }
            }
            expr = std::move(obj_keys);
            type = type_Link;
        }
    };

    if (left_type == type_Link && right->has_constant_evaluation()) {
        handle_typed_links(left, right, right_type);
    }
    if (right_type == type_Link && left->has_constant_evaluation()) {
        handle_typed_links(right, left, left_type);
    }

    if (left_type.is_valid() && right_type.is_valid() && !Mixed::data_types_are_comparable(left_type, right_type)) {
        throw InvalidQueryError(util::format("Unsupported comparison between type '%1' and type '%2'",
                                             get_data_type_name(left_type), get_data_type_name(right_type)));
    }
    if (left_type == type_TypeOfValue || right_type == type_TypeOfValue) {
        if (left_type != right_type) {
            throw InvalidQueryArgError(
                util::format("Unsupported comparison between @type and raw value: '%1' and '%2'",
                             get_data_type_name(left_type), get_data_type_name(right_type)));
        }
    }

    if (op == CompareType::IN) {
        Subexpr* r = right.get();
        if (!r->has_multiple_values()) {
            throw InvalidQueryArgError("The keypath following 'IN' must contain a list. Found '" +
                                       r->description(drv->m_serializer_state) + "'");
        }
    }

    if (left_type == type_Link && left_type == right_type && right->has_constant_evaluation()) {
        if (auto link_column = dynamic_cast<const Columns<Link>*>(left.get())) {
            if (link_column->link_map().get_nb_hops() == 1 &&
                link_column->get_comparison_type().value_or(ExpressionComparisonType::Any) ==
                    ExpressionComparisonType::Any) {
                REALM_ASSERT(dynamic_cast<const Value<ObjKey>*>(right.get()));
                auto link_values = static_cast<const Value<ObjKey>*>(right.get());
                // We can use a LinksToNode based query
                std::vector<ObjKey> values;
                values.reserve(link_values->size());
                for (auto val : *link_values) {
                    values.emplace_back(val.is_null() ? ObjKey() : val.get<ObjKey>());
                }
                if (op == CompareType::EQUAL) {
                    return drv->m_base_table->where().links_to(link_column->link_map().get_first_column_key(),
                                                               values);
                }
                else if (op == CompareType::NOT_EQUAL) {
                    return drv->m_base_table->where().not_links_to(link_column->link_map().get_first_column_key(),
                                                                   values);
                }
            }
        }
    }
    else if (right->has_single_value() && (left_type == right_type || left_type == type_Mixed)) {
        Mixed val = right->get_mixed();
        const ObjPropertyBase* prop = dynamic_cast<const ObjPropertyBase*>(left.get());
        if (prop && !prop->links_exist()) {
            auto col_key = prop->column_key();
            if (val.is_null()) {
                switch (op) {
                    case CompareType::EQUAL:
                    case CompareType::IN:
                        return drv->m_base_table->where().equal(col_key, realm::null());
                    case CompareType::NOT_EQUAL:
                        return drv->m_base_table->where().not_equal(col_key, realm::null());
                    default:
                        break;
                }
            }
            switch (left->get_type()) {
                case type_Int:
                    return drv->simple_query(op, col_key, val.get_int());
                case type_Bool:
                    return drv->simple_query(op, col_key, val.get_bool());
                case type_String:
                    return drv->simple_query(op, col_key, val.get_string(), case_sensitive);
                case type_Binary:
                    return drv->simple_query(op, col_key, val.get_binary(), case_sensitive);
                case type_Timestamp:
                    return drv->simple_query(op, col_key, val.get<Timestamp>());
                case type_Float:
                    return drv->simple_query(op, col_key, val.get_float());
                case type_Double:
                    return drv->simple_query(op, col_key, val.get_double());
                case type_Decimal:
                    return drv->simple_query(op, col_key, val.get<Decimal128>());
                case type_ObjectId:
                    return drv->simple_query(op, col_key, val.get<ObjectId>());
                case type_UUID:
                    return drv->simple_query(op, col_key, val.get<UUID>());
                case type_Mixed:
                    return drv->simple_query(op, col_key, val, case_sensitive);
                default:
                    break;
            }
        }
    }
    if (case_sensitive) {
        switch (op) {
            case CompareType::EQUAL:
            case CompareType::IN:
                return Query(std::unique_ptr<Expression>(new Compare<Equal>(std::move(left), std::move(right))));
            case CompareType::NOT_EQUAL:
                return Query(std::unique_ptr<Expression>(new Compare<NotEqual>(std::move(left), std::move(right))));
            default:
                break;
        }
    }
    else {
        verify_only_string_types(right_type, util::format("%1%2", string_for_op(op), "[c]"));
        switch (op) {
            case CompareType::EQUAL:
            case CompareType::IN:
                return Query(std::unique_ptr<Expression>(new Compare<EqualIns>(std::move(left), std::move(right))));
            case CompareType::NOT_EQUAL:
                return Query(
                    std::unique_ptr<Expression>(new Compare<NotEqualIns>(std::move(left), std::move(right))));
            default:
                break;
        }
    }
    return {};
}

Query BetweenNode::visit(ParserDriver* drv)
{
    if (limits->elements.size() != 2) {
        throw InvalidQueryError("Operator 'BETWEEN' requires list with 2 elements.");
    }

    if (dynamic_cast<ColumnListBase*>(prop->visit(drv, type_Int).get())) {
        // It's a list!
        util::Optional<ExpressionComparisonType> cmp_type = dynamic_cast<PropertyNode*>(prop)->comp_type;
        if (cmp_type.value_or(ExpressionComparisonType::Any) != ExpressionComparisonType::All) {
            throw InvalidQueryError("Only 'ALL' supported for operator 'BETWEEN' when applied to lists.");
        }
    }

    auto& min(limits->elements.at(0));
    auto& max(limits->elements.at(1));
    RelationalNode cmp1(prop, CompareType::GREATER_EQUAL, min);
    RelationalNode cmp2(prop, CompareType::LESS_EQUAL, max);

    Query q(drv->m_base_table);
    q.and_query(cmp1.visit(drv));
    q.and_query(cmp2.visit(drv));

    return q;
}

Query RelationalNode::visit(ParserDriver* drv)
{
    auto [left, right] = drv->cmp(values);

    auto left_type = left->get_type();
    auto right_type = right->get_type();
    const bool right_type_is_null = right->has_single_value() && right->get_mixed().is_null();
    const bool left_type_is_null = left->has_single_value() && left->get_mixed().is_null();
    REALM_ASSERT(!(left_type_is_null && right_type_is_null));

    if (left_type == type_Link || left_type == type_TypeOfValue) {
        throw InvalidQueryError(util::format(
            "Unsupported operator %1 in query. Only equal (==) and not equal (!=) are supported for this type.",
            string_for_op(op)));
    }

    if (!(left_type_is_null || right_type_is_null) && (!left_type.is_valid() || !right_type.is_valid() ||
                                                       !Mixed::data_types_are_comparable(left_type, right_type))) {
        throw InvalidQueryError(util::format("Unsupported comparison between type '%1' and type '%2'",
                                             get_data_type_name(left_type), get_data_type_name(right_type)));
    }

    const ObjPropertyBase* prop = dynamic_cast<const ObjPropertyBase*>(left.get());
    if (prop && !prop->links_exist() && right->has_single_value() &&
        (left_type == right_type || left_type == type_Mixed)) {
        auto col_key = prop->column_key();
        switch (left->get_type()) {
            case type_Int:
                return drv->simple_query(op, col_key, right->get_mixed().get_int());
            case type_Bool:
                break;
            case type_String:
                break;
            case type_Binary:
                break;
            case type_Timestamp:
                return drv->simple_query(op, col_key, right->get_mixed().get<Timestamp>());
            case type_Float:
                return drv->simple_query(op, col_key, right->get_mixed().get_float());
                break;
            case type_Double:
                return drv->simple_query(op, col_key, right->get_mixed().get_double());
                break;
            case type_Decimal:
                return drv->simple_query(op, col_key, right->get_mixed().get<Decimal128>());
                break;
            case type_ObjectId:
                return drv->simple_query(op, col_key, right->get_mixed().get<ObjectId>());
                break;
            case type_UUID:
                return drv->simple_query(op, col_key, right->get_mixed().get<UUID>());
                break;
            case type_Mixed:
                return drv->simple_query(op, col_key, right->get_mixed());
                break;
            default:
                break;
        }
    }
    switch (op) {
        case CompareType::GREATER:
            return Query(std::unique_ptr<Expression>(new Compare<Greater>(std::move(left), std::move(right))));
        case CompareType::LESS:
            return Query(std::unique_ptr<Expression>(new Compare<Less>(std::move(left), std::move(right))));
        case CompareType::GREATER_EQUAL:
            return Query(std::unique_ptr<Expression>(new Compare<GreaterEqual>(std::move(left), std::move(right))));
        case CompareType::LESS_EQUAL:
            return Query(std::unique_ptr<Expression>(new Compare<LessEqual>(std::move(left), std::move(right))));
        default:
            break;
    }
    return {};
}

Query StringOpsNode::visit(ParserDriver* drv)
{
    auto [left, right] = drv->cmp(values);

    auto left_type = left->get_type();
    auto right_type = right->get_type();
    const ObjPropertyBase* prop = dynamic_cast<const ObjPropertyBase*>(left.get());

    verify_only_string_types(right_type, string_for_op(op));

    if (prop && !prop->links_exist() && right->has_single_value() &&
        (left_type == right_type || left_type == type_Mixed)) {
        auto col_key = prop->column_key();
        if (right_type == type_String) {
            StringData val = right->get_mixed().get_string();

            switch (op) {
                case CompareType::BEGINSWITH:
                    return drv->m_base_table->where().begins_with(col_key, val, case_sensitive);
                case CompareType::ENDSWITH:
                    return drv->m_base_table->where().ends_with(col_key, val, case_sensitive);
                case CompareType::CONTAINS:
                    return drv->m_base_table->where().contains(col_key, val, case_sensitive);
                case CompareType::LIKE:
                    return drv->m_base_table->where().like(col_key, val, case_sensitive);
                case CompareType::TEXT:
                    return drv->m_base_table->where().fulltext(col_key, val);
                case CompareType::IN:
                case CompareType::EQUAL:
                case CompareType::NOT_EQUAL:
                case CompareType::GREATER:
                case CompareType::LESS:
                case CompareType::GREATER_EQUAL:
                case CompareType::LESS_EQUAL:
                    break;
            }
        }
        else if (right_type == type_Binary) {
            BinaryData val = right->get_mixed().get_binary();

            switch (op) {
                case CompareType::BEGINSWITH:
                    return drv->m_base_table->where().begins_with(col_key, val, case_sensitive);
                case CompareType::ENDSWITH:
                    return drv->m_base_table->where().ends_with(col_key, val, case_sensitive);
                case CompareType::CONTAINS:
                    return drv->m_base_table->where().contains(col_key, val, case_sensitive);
                case CompareType::LIKE:
                    return drv->m_base_table->where().like(col_key, val, case_sensitive);
                case CompareType::TEXT:
                case CompareType::IN:
                case CompareType::EQUAL:
                case CompareType::NOT_EQUAL:
                case CompareType::GREATER:
                case CompareType::LESS:
                case CompareType::GREATER_EQUAL:
                case CompareType::LESS_EQUAL:
                    break;
            }
        }
    }

    if (case_sensitive) {
        switch (op) {
            case CompareType::BEGINSWITH:
                return Query(std::unique_ptr<Expression>(new Compare<BeginsWith>(std::move(right), std::move(left))));
            case CompareType::ENDSWITH:
                return Query(std::unique_ptr<Expression>(new Compare<EndsWith>(std::move(right), std::move(left))));
            case CompareType::CONTAINS:
                return Query(std::unique_ptr<Expression>(new Compare<Contains>(std::move(right), std::move(left))));
            case CompareType::LIKE:
                return Query(std::unique_ptr<Expression>(new Compare<Like>(std::move(right), std::move(left))));
            case CompareType::TEXT: {
                StringData val = right->get_mixed().get_string();
                auto string_prop = dynamic_cast<Columns<StringData>*>(left.get());
                return string_prop->fulltext(val);
            }
            case CompareType::IN:
            case CompareType::EQUAL:
            case CompareType::NOT_EQUAL:
            case CompareType::GREATER:
            case CompareType::LESS:
            case CompareType::GREATER_EQUAL:
            case CompareType::LESS_EQUAL:
                break;
        }
    }
    else {
        switch (op) {
            case CompareType::BEGINSWITH:
                return Query(
                    std::unique_ptr<Expression>(new Compare<BeginsWithIns>(std::move(right), std::move(left))));
            case CompareType::ENDSWITH:
                return Query(
                    std::unique_ptr<Expression>(new Compare<EndsWithIns>(std::move(right), std::move(left))));
            case CompareType::CONTAINS:
                return Query(
                    std::unique_ptr<Expression>(new Compare<ContainsIns>(std::move(right), std::move(left))));
            case CompareType::LIKE:
                return Query(std::unique_ptr<Expression>(new Compare<LikeIns>(std::move(right), std::move(left))));
            case CompareType::IN:
            case CompareType::EQUAL:
            case CompareType::NOT_EQUAL:
            case CompareType::GREATER:
            case CompareType::LESS:
            case CompareType::GREATER_EQUAL:
            case CompareType::LESS_EQUAL:
            case CompareType::TEXT:
                break;
        }
    }
    return {};
}

#if REALM_ENABLE_GEOSPATIAL
Query GeoWithinNode::visit(ParserDriver* drv)
{
    auto left = prop->visit(drv);
    auto left_type = left->get_type();
    if (left_type != type_Link) {
        throw InvalidQueryError(util::format("The left hand side of 'geoWithin' must be a link to geoJSON formatted "
                                             "data. But the provided type is '%1'",
                                             get_data_type_name(left_type)));
    }
    auto link_column = dynamic_cast<const Columns<Link>*>(left.get());

    if (geo) {
        auto right = geo->visit(drv, type_Int);
        auto geo_value = dynamic_cast<const ConstantGeospatialValue*>(right.get());
        return link_column->geo_within(geo_value->get_mixed().get<Geospatial>());
    }

    REALM_ASSERT_3(argument.size(), >, 1);
    REALM_ASSERT_3(argument[0], ==, '$');
    size_t arg_no = size_t(strtol(argument.substr(1).c_str(), nullptr, 10));
    auto right_type = drv->m_args.is_argument_null(arg_no) ? DataType(-1) : drv->m_args.type_for_argument(arg_no);

    Geospatial geo_from_argument;
    if (right_type == type_Geospatial) {
        geo_from_argument = drv->m_args.geospatial_for_argument(arg_no);
    }
    else if (right_type == type_String) {
        // This is a "hack" to allow users to pass in geospatial objects
        // serialized as a string instead of as a native type. This is because
        // the CAPI doesn't have support for marshalling polygons (of variable length)
        // yet and that project was deprioritized to geospatial phase 2. This should be
        // removed once SDKs are all using the binding generator.
        std::string str_val = drv->m_args.string_for_argument(arg_no);
        const std::string simulated_prefix = "simulated GEOWITHIN ";
        str_val = simulated_prefix + str_val;
        ParserDriver sub_driver;
        try {
            sub_driver.parse(str_val);
        }
        catch (const std::exception& ex) {
            std::string doctored_err = ex.what();
            size_t prefix_location = doctored_err.find(simulated_prefix);
            if (prefix_location != std::string::npos) {
                doctored_err.erase(prefix_location, simulated_prefix.size());
            }
            throw InvalidQueryError(util::format(
                "Invalid syntax in serialized geospatial object at argument %1: '%2'", arg_no, doctored_err));
        }
        GeoWithinNode* node = dynamic_cast<GeoWithinNode*>(sub_driver.result);
        REALM_ASSERT(node);
        if (node->geo) {
            if (node->geo->m_geo.get_type() != Geospatial::Type::Invalid) {
                geo_from_argument = node->geo->m_geo;
            }
            else {
                geo_from_argument = GeoPolygon{node->geo->m_points};
            }
        }
    }
    else {
        throw InvalidQueryError(util::format("The right hand side of 'geoWithin' must be a geospatial constant "
                                             "value. But the provided type is '%1'",
                                             get_data_type_name(right_type)));
    }

    if (geo_from_argument.get_type() == Geospatial::Type::Invalid) {
        throw InvalidQueryError(util::format(
            "The right hand side of 'geoWithin' must be a valid Geospatial value, got '%1'", geo_from_argument));
    }
    Status geo_status = geo_from_argument.is_valid();
    if (!geo_status.is_ok()) {
        throw InvalidQueryError(
            util::format("The Geospatial query argument region is invalid: '%1'", geo_status.reason()));
    }
    return link_column->geo_within(geo_from_argument);
}
#endif

Query TrueOrFalseNode::visit(ParserDriver* drv)
{
    Query q = drv->m_base_table->where();
    if (true_or_false) {
        q.and_query(std::unique_ptr<realm::Expression>(new TrueExpression));
    }
    else {
        q.and_query(std::unique_ptr<realm::Expression>(new FalseExpression));
    }
    return q;
}

std::unique_ptr<Subexpr> PropertyNode::visit(ParserDriver* drv, DataType)
{
    bool is_keys = false;
    std::string identifier = path->path_elems.back().id;
    if (identifier[0] == '@') {
        if (identifier == "@values") {
            path->path_elems.pop_back();
            identifier = path->path_elems.back().id;
        }
        else if (identifier == "@keys") {
            path->path_elems.pop_back();
            identifier = path->path_elems.back().id;
            is_keys = true;
        }
        else if (identifier == "@links") {
            // This is a backlink aggregate query
            auto link_chain = path->visit(drv, comp_type);
            auto sub = link_chain.get_backlink_count<Int>();
            return sub.clone();
        }
    }
    try {
        m_link_chain = path->visit(drv, comp_type);
        std::unique_ptr<Subexpr> subexpr;
        if (is_keys || !path->path_elems.back().index.is_null()) {
            // It must be a dictionary
            subexpr = drv->dictionary_column(m_link_chain, identifier);
            auto s = dynamic_cast<Columns<Dictionary>*>(subexpr.get());
            if (!s) {
                throw InvalidQueryError("Not a dictionary");
            }
            if (is_keys) {
                subexpr = std::make_unique<ColumnDictionaryKeys>(*s);
            }
            else {
                subexpr = s->key(path->path_elems.back().index).clone();
            }
        }
        else {
            subexpr = drv->column(m_link_chain, identifier);
        }

        if (post_op) {
            return post_op->visit(drv, subexpr.get());
        }
        return subexpr;
    }
    catch (const InvalidQueryError&) {
        // Is 'identifier' perhaps length operator?
        if (!post_op && is_length_suffix(identifier) && path->path_elems.size() > 1) {
            // If 'length' is the operator, the last id in the path must be the name
            // of a list property
            path->path_elems.pop_back();
            std::string& prop = path->path_elems.back().id;
            std::unique_ptr<Subexpr> subexpr{path->visit(drv, comp_type).column(prop)};
            if (auto list = dynamic_cast<ColumnListBase*>(subexpr.get())) {
                if (auto length_expr = list->get_element_length())
                    return length_expr;
            }
        }
        throw;
    }
    REALM_UNREACHABLE();
    return {};
}

std::unique_ptr<Subexpr> SubqueryNode::visit(ParserDriver* drv, DataType)
{
    if (variable_name.size() < 2 || variable_name[0] != '$') {
        throw SyntaxError(util::format("The subquery variable '%1' is invalid. The variable must start with "
                                       "'$' and cannot be empty; for example '$x'.",
                                       variable_name));
    }
    LinkChain lc = prop->path->visit(drv, prop->comp_type);
    std::string identifier = prop->path->path_elems.back().id;
    identifier = drv->translate(lc, identifier);

    if (identifier.find("@links") == 0) {
        drv->backlink(lc, identifier);
    }
    else {
        ColKey col_key = lc.get_current_table()->get_column_key(identifier);
        if (col_key.is_list() && col_key.get_type() != col_type_LinkList) {
            throw InvalidQueryError(
                util::format("A subquery can not operate on a list of primitive values (property '%1')", identifier));
        }
        if (col_key.get_type() != col_type_LinkList) {
            throw InvalidQueryError(util::format("A subquery must operate on a list property, but '%1' is type '%2'",
                                                 identifier,
                                                 realm::get_data_type_name(DataType(col_key.get_type()))));
        }
        lc.link(identifier);
    }
    TableRef previous_table = drv->m_base_table;
    drv->m_base_table = lc.get_current_table().cast_away_const();
    bool did_add = drv->m_mapping.add_mapping(drv->m_base_table, variable_name, "");
    if (!did_add) {
        throw InvalidQueryError(util::format("Unable to create a subquery expression with variable '%1' since an "
                                             "identical variable already exists in this context",
                                             variable_name));
    }
    Query sub = subquery->visit(drv);
    drv->m_mapping.remove_mapping(drv->m_base_table, variable_name);
    drv->m_base_table = previous_table;

    return lc.subquery(sub);
}

std::unique_ptr<Subexpr> PostOpNode::visit(ParserDriver*, Subexpr* subexpr)
{
    if (op_type == PostOpNode::SIZE) {
        if (auto s = dynamic_cast<Columns<Link>*>(subexpr)) {
            return s->count().clone();
        }
        if (auto s = dynamic_cast<ColumnListBase*>(subexpr)) {
            return s->size().clone();
        }
        if (auto s = dynamic_cast<Columns<StringData>*>(subexpr)) {
            return s->size().clone();
        }
        if (auto s = dynamic_cast<Columns<BinaryData>*>(subexpr)) {
            return s->size().clone();
        }
    }
    else if (op_type == PostOpNode::TYPE) {
        if (auto s = dynamic_cast<Columns<Mixed>*>(subexpr)) {
            return s->type_of_value().clone();
        }
        if (auto s = dynamic_cast<ColumnsCollection<Mixed>*>(subexpr)) {
            return s->type_of_value().clone();
        }
        if (auto s = dynamic_cast<ObjPropertyBase*>(subexpr)) {
            return Value<TypeOfValue>(TypeOfValue(s->column_key())).clone();
        }
        if (dynamic_cast<Columns<Link>*>(subexpr)) {
            return Value<TypeOfValue>(TypeOfValue(TypeOfValue::Attribute::ObjectLink)).clone();
        }
    }

    if (subexpr) {
        throw InvalidQueryError(util::format("Operation '%1' is not supported on property of type '%2'", op_name,
                                             get_data_type_name(DataType(subexpr->get_type()))));
    }
    REALM_UNREACHABLE();
    return {};
}

std::unique_ptr<Subexpr> LinkAggrNode::visit(ParserDriver* drv, DataType)
{
    auto subexpr = property->visit(drv);
    auto link_prop = dynamic_cast<Columns<Link>*>(subexpr.get());
    if (!link_prop) {
        throw InvalidQueryError(util::format("Operation '%1' cannot apply to property '%2' because it is not a list",
                                             agg_op_type_to_str(type), property->identifier()));
    }
    const LinkChain& link_chain = property->link_chain();
    prop_name = drv->translate(link_chain, prop_name);
    auto col_key = link_chain.get_current_table()->get_column_key(prop_name);

    switch (col_key.get_type()) {
        case col_type_Int:
            subexpr = link_prop->column<Int>(col_key).clone();
            break;
        case col_type_Float:
            subexpr = link_prop->column<float>(col_key).clone();
            break;
        case col_type_Double:
            subexpr = link_prop->column<double>(col_key).clone();
            break;
        case col_type_Decimal:
            subexpr = link_prop->column<Decimal>(col_key).clone();
            break;
        case col_type_Timestamp:
            subexpr = link_prop->column<Timestamp>(col_key).clone();
            break;
        default:
            throw InvalidQueryError(util::format("collection aggregate not supported for type '%1'",
                                                 get_data_type_name(DataType(col_key.get_type()))));
    }
    return aggregate(subexpr.get());
}

std::unique_ptr<Subexpr> ListAggrNode::visit(ParserDriver* drv, DataType)
{
    auto subexpr = property->visit(drv);
    return aggregate(subexpr.get());
}

std::unique_ptr<Subexpr> AggrNode::aggregate(Subexpr* subexpr)
{
    std::unique_ptr<Subexpr> agg;
    if (auto list_prop = dynamic_cast<ColumnListBase*>(subexpr)) {
        switch (type) {
            case MAX:
                agg = list_prop->max_of();
                break;
            case MIN:
                agg = list_prop->min_of();
                break;
            case SUM:
                agg = list_prop->sum_of();
                break;
            case AVG:
                agg = list_prop->avg_of();
                break;
        }
    }
    else if (auto prop = dynamic_cast<SubColumnBase*>(subexpr)) {
        switch (type) {
            case MAX:
                agg = prop->max_of();
                break;
            case MIN:
                agg = prop->min_of();
                break;
            case SUM:
                agg = prop->sum_of();
                break;
            case AVG:
                agg = prop->avg_of();
                break;
        }
    }
    if (!agg) {
        throw InvalidQueryError(
            util::format("Cannot use aggregate '%1' for this type of property", agg_op_type_to_str(type)));
    }

    return agg;
}

std::unique_ptr<Subexpr> ConstantNode::visit(ParserDriver* drv, DataType hint)
{
    std::unique_ptr<Subexpr> ret;
    std::string explain_value_message = text;
    if (target_table.length()) {
        const Group* g = drv->m_base_table->get_parent_group();
        TableKey table_key;
        ObjKey obj_key;
        auto table = g->get_table(target_table);
        if (!table) {
            // Perhaps class prefix is missing
            Group::TableNameBuffer buffer;
            table = g->get_table(Group::class_name_to_table_name(target_table, buffer));
        }
        if (!table) {
            throw InvalidQueryError(util::format("Unknown object type '%1'", target_table));
        }
        table_key = table->get_key();
        target_table = "";
        auto pk_val_node = visit(drv, hint); // call recursively
        auto pk_val = pk_val_node->get_mixed();
        obj_key = table->find_primary_key(pk_val);
        return std::make_unique<Value<ObjLink>>(ObjLink(table_key, ObjKey(obj_key)));
    }
    switch (type) {
        case Type::NUMBER: {
            if (hint == type_Decimal) {
                ret = std::make_unique<Value<Decimal128>>(Decimal128(text));
            }
            else {
                ret = std::make_unique<Value<int64_t>>(strtoll(text.c_str(), nullptr, 0));
            }
            break;
        }
        case Type::FLOAT: {
            if (hint == type_Float || text[text.size() - 1] == 'f') {
                ret = std::make_unique<Value<float>>(strtof(text.c_str(), nullptr));
            }
            else if (hint == type_Decimal) {
                ret = std::make_unique<Value<Decimal128>>(Decimal128(text));
            }
            else {
                ret = std::make_unique<Value<double>>(strtod(text.c_str(), nullptr));
            }
            break;
        }
        case Type::INFINITY_VAL: {
            bool negative = text[0] == '-';
            switch (hint) {
                case type_Float: {
                    auto inf = std::numeric_limits<float>::infinity();
                    ret = std::make_unique<Value<float>>(negative ? -inf : inf);
                    break;
                }
                case type_Double: {
                    auto inf = std::numeric_limits<double>::infinity();
                    ret = std::make_unique<Value<double>>(negative ? -inf : inf);
                    break;
                }
                case type_Decimal:
                    ret = std::make_unique<Value<Decimal128>>(Decimal128(text));
                    break;
                default:
                    throw InvalidQueryError(util::format("Infinity not supported for %1", get_data_type_name(hint)));
                    break;
            }
            break;
        }
        case Type::NAN_VAL: {
            switch (hint) {
                case type_Float:
                    ret = std::make_unique<Value<float>>(type_punning<float>(0x7fc00000));
                    break;
                case type_Double:
                    ret = std::make_unique<Value<double>>(type_punning<double>(0x7ff8000000000000));
                    break;
                case type_Decimal:
                    ret = std::make_unique<Value<Decimal128>>(Decimal128::nan("0"));
                    break;
                default:
                    REALM_UNREACHABLE();
                    break;
            }
            break;
        }
        case Type::STRING: {
            std::string str = text.substr(1, text.size() - 2);
            switch (hint) {
                case type_Int:
                    ret = std::make_unique<Value<int64_t>>(string_to<int64_t>(str));
                    break;
                case type_Float:
                    ret = std::make_unique<Value<float>>(string_to<float>(str));
                    break;
                case type_Double:
                    ret = std::make_unique<Value<double>>(string_to<double>(str));
                    break;
                case type_Decimal:
                    ret = std::make_unique<Value<Decimal128>>(Decimal128(str.c_str()));
                    break;
                default:
                    if (hint == type_TypeOfValue) {
                        ret = std::make_unique<Value<TypeOfValue>>(TypeOfValue(str));
                    }
                    else {
                        ret = std::make_unique<ConstantStringValue>(str);
                    }
                    break;
            }
            break;
        }
        case Type::BASE64: {
            const size_t encoded_size = text.size() - 5;
            size_t buffer_size = util::base64_decoded_size(encoded_size);
            std::string decode_buffer(buffer_size, char(0));
            StringData window(text.c_str() + 4, encoded_size);
            util::Optional<size_t> decoded_size = util::base64_decode(window, decode_buffer.data(), buffer_size);
            if (!decoded_size) {
                throw SyntaxError("Invalid base64 value");
            }
            REALM_ASSERT_DEBUG_EX(*decoded_size <= encoded_size, *decoded_size, encoded_size);
            decode_buffer.resize(*decoded_size); // truncate
            if (hint == type_String) {
                ret = std::make_unique<ConstantStringValue>(StringData(decode_buffer.data(), decode_buffer.size()));
            }
            if (hint == type_Binary) {
                ret = std::make_unique<ConstantBinaryValue>(BinaryData(decode_buffer.data(), decode_buffer.size()));
            }
            if (hint == type_Mixed) {
                ret = std::make_unique<ConstantBinaryValue>(BinaryData(decode_buffer.data(), decode_buffer.size()));
            }
            break;
        }
        case Type::TIMESTAMP: {
            auto s = text;
            int64_t seconds;
            int32_t nanoseconds;
            if (s[0] == 'T') {
                size_t colon_pos = s.find(":");
                std::string s1 = s.substr(1, colon_pos - 1);
                std::string s2 = s.substr(colon_pos + 1);
                seconds = strtol(s1.c_str(), nullptr, 0);
                nanoseconds = int32_t(strtol(s2.c_str(), nullptr, 0));
            }
            else {
                // readable format YYYY-MM-DD-HH:MM:SS:NANOS nanos optional
                struct tm tmp = tm();
                char sep = s.find("@") < s.size() ? '@' : 'T';
                std::string fmt = "%d-%d-%d"s + sep + "%d:%d:%d:%d"s;
                int cnt = sscanf(s.c_str(), fmt.c_str(), &tmp.tm_year, &tmp.tm_mon, &tmp.tm_mday, &tmp.tm_hour,
                                 &tmp.tm_min, &tmp.tm_sec, &nanoseconds);
                REALM_ASSERT(cnt >= 6);
                tmp.tm_year -= 1900; // epoch offset (see man mktime)
                tmp.tm_mon -= 1;     // converts from 1-12 to 0-11

                if (tmp.tm_year < 0) {
                    // platform timegm functions do not throw errors, they return -1 which is also a valid time
                    throw InvalidQueryError("Conversion of dates before 1900 is not supported.");
                }

                seconds = platform_timegm(tmp); // UTC time
                if (cnt == 6) {
                    nanoseconds = 0;
                }
                if (nanoseconds < 0) {
                    throw SyntaxError("The nanoseconds of a Timestamp cannot be negative.");
                }
                if (seconds < 0) { // seconds determines the sign of the nanoseconds part
                    nanoseconds *= -1;
                }
            }
            ret = std::make_unique<Value<Timestamp>>(get_timestamp_if_valid(seconds, nanoseconds));
            break;
        }
        case Type::UUID_T:
            ret = std::make_unique<Value<UUID>>(UUID(text.substr(5, text.size() - 6)));
            break;
        case Type::OID:
            ret = std::make_unique<Value<ObjectId>>(ObjectId(text.substr(4, text.size() - 5).c_str()));
            break;
        case Type::LINK: {
            ret =
                std::make_unique<Value<ObjKey>>(ObjKey(strtol(text.substr(1, text.size() - 1).c_str(), nullptr, 0)));
            break;
        }
        case Type::TYPED_LINK: {
            size_t colon_pos = text.find(":");
            auto table_key_val = uint32_t(strtol(text.substr(1, colon_pos - 1).c_str(), nullptr, 0));
            auto obj_key_val = strtol(text.substr(colon_pos + 1).c_str(), nullptr, 0);
            ret = std::make_unique<Value<ObjLink>>(ObjLink(TableKey(table_key_val), ObjKey(obj_key_val)));
            break;
        }
        case Type::NULL_VAL:
            if (hint == type_String) {
                ret = std::make_unique<ConstantStringValue>(StringData()); // Null string
            }
            else if (hint == type_Binary) {
                ret = std::make_unique<Value<Binary>>(BinaryData()); // Null string
            }
            else {
                ret = std::make_unique<Value<null>>(realm::null());
            }
            break;
        case Type::TRUE:
            ret = std::make_unique<Value<Bool>>(true);
            break;
        case Type::FALSE:
            ret = std::make_unique<Value<Bool>>(false);
            break;
        case Type::ARG: {
            size_t arg_no = size_t(strtol(text.substr(1).c_str(), nullptr, 10));
            if (m_comp_type && !drv->m_args.is_argument_list(arg_no)) {
                throw InvalidQueryError(util::format(
                    "ANY/ALL/NONE are only allowed on arguments which contain a list but '%1' is not a list.",
                    explain_value_message));
            }
            if (drv->m_args.is_argument_null(arg_no)) {
                explain_value_message = util::format("argument '%1' which is NULL", explain_value_message);
                ret = std::make_unique<Value<null>>(realm::null());
            }
            else if (drv->m_args.is_argument_list(arg_no)) {
                std::vector<Mixed> mixed_list = drv->m_args.list_for_argument(arg_no);
                ret = copy_list_of_args(mixed_list);
            }
            else {
                auto type = drv->m_args.type_for_argument(arg_no);
                explain_value_message =
                    util::format("argument %1 of type '%2'", explain_value_message, get_data_type_name(type));
                ret = copy_arg(drv, type, arg_no, hint, explain_value_message);
            }
            break;
        }
    }
    if (!ret) {
        throw InvalidQueryError(
            util::format("Unsupported comparison between property of type '%1' and constant value: %2",
                         get_data_type_name(hint), explain_value_message));
    }
    return ret;
}

std::unique_ptr<ConstantMixedList> ConstantNode::copy_list_of_args(std::vector<Mixed>& mixed_args)
{
    std::unique_ptr<ConstantMixedList> args_in_list = std::make_unique<ConstantMixedList>(mixed_args.size());
    size_t ndx = 0;
    for (const auto& mixed : mixed_args) {
        args_in_list->set(ndx++, mixed);
    }
    if (m_comp_type) {
        args_in_list->set_comparison_type(*m_comp_type);
    }
    return args_in_list;
}

std::unique_ptr<Subexpr> ConstantNode::copy_arg(ParserDriver* drv, DataType type, size_t arg_no, DataType hint,
                                                std::string& err)
{
    switch (type) {
        case type_Int:
            return std::make_unique<Value<int64_t>>(drv->m_args.long_for_argument(arg_no));
        case type_String:
            return std::make_unique<ConstantStringValue>(drv->m_args.string_for_argument(arg_no));
        case type_Binary:
            return std::make_unique<ConstantBinaryValue>(drv->m_args.binary_for_argument(arg_no));
        case type_Bool:
            return std::make_unique<Value<Bool>>(drv->m_args.bool_for_argument(arg_no));
        case type_Float:
            return std::make_unique<Value<float>>(drv->m_args.float_for_argument(arg_no));
        case type_Double: {
            // In realm-js all number type arguments are returned as double. If we don't cast to the
            // expected type, we would in many cases miss the option to use the optimized query node
            // instead of the general Compare class.
            double val = drv->m_args.double_for_argument(arg_no);
            switch (hint) {
                case type_Int:
                case type_Bool: {
                    int64_t int_val = int64_t(val);
                    // Only return an integer if it precisely represents val
                    if (double(int_val) == val)
                        return std::make_unique<Value<int64_t>>(int_val);
                    else
                        return std::make_unique<Value<double>>(val);
                }
                case type_Float:
                    return std::make_unique<Value<float>>(float(val));
                default:
                    return std::make_unique<Value<double>>(val);
            }
            break;
        }
        case type_Timestamp: {
            try {
                return std::make_unique<Value<Timestamp>>(drv->m_args.timestamp_for_argument(arg_no));
            }
            catch (const std::exception&) {
                return std::make_unique<Value<ObjectId>>(drv->m_args.objectid_for_argument(arg_no));
            }
        }
        case type_ObjectId: {
            try {
                return std::make_unique<Value<ObjectId>>(drv->m_args.objectid_for_argument(arg_no));
            }
            catch (const std::exception&) {
                return std::make_unique<Value<Timestamp>>(drv->m_args.timestamp_for_argument(arg_no));
            }
            break;
        }
        case type_Decimal:
            return std::make_unique<Value<Decimal128>>(drv->m_args.decimal128_for_argument(arg_no));
        case type_UUID:
            return std::make_unique<Value<UUID>>(drv->m_args.uuid_for_argument(arg_no));
        case type_Link:
            return std::make_unique<Value<ObjKey>>(drv->m_args.object_index_for_argument(arg_no));
        case type_TypedLink:
            if (hint == type_Mixed || hint == type_Link || hint == type_TypedLink) {
                return std::make_unique<Value<ObjLink>>(drv->m_args.objlink_for_argument(arg_no));
            }
            err = util::format("%1 which links to %2", err,
                               print_pretty_objlink(drv->m_args.objlink_for_argument(arg_no),
                                                    drv->m_base_table->get_parent_group()));
            break;
        default:
            break;
    }
    return nullptr;
}

#if REALM_ENABLE_GEOSPATIAL
GeospatialNode::GeospatialNode(GeospatialNode::Box, GeoPoint& p1, GeoPoint& p2)
    : m_geo{Geospatial{GeoBox{p1, p2}}}
{
}

GeospatialNode::GeospatialNode(Circle, GeoPoint& p, double radius)
    : m_geo{Geospatial{GeoCircle{radius, p}}}
{
}

GeospatialNode::GeospatialNode(Polygon, GeoPoint& p)
    : m_points({{p}})
{
}

GeospatialNode::GeospatialNode(Loop, GeoPoint& p)
    : m_points({{p}})
{
}

void GeospatialNode::add_point_to_loop(GeoPoint& p)
{
    m_points.back().push_back(p);
}

void GeospatialNode::add_loop_to_polygon(GeospatialNode* node)
{
    m_points.push_back(node->m_points.back());
}

std::unique_ptr<Subexpr> GeospatialNode::visit(ParserDriver*, DataType)
{
    std::unique_ptr<Subexpr> ret;
    if (m_geo.get_type() != Geospatial::Type::Invalid) {
        ret = std::make_unique<ConstantGeospatialValue>(m_geo);
    }
    else {
        ret = std::make_unique<ConstantGeospatialValue>(GeoPolygon{m_points});
    }
    return ret;
}
#endif

std::unique_ptr<Subexpr> ListNode::visit(ParserDriver* drv, DataType hint)
{
    if (hint == type_TypeOfValue) {
        try {
            std::unique_ptr<Value<TypeOfValue>> ret = std::make_unique<Value<TypeOfValue>>();
            constexpr bool is_list = true;
            ret->init(is_list, elements.size());
            ret->set_comparison_type(m_comp_type);
            size_t ndx = 0;
            for (auto constant : elements) {
                std::unique_ptr<Subexpr> evaluated = constant->visit(drv, hint);
                if (auto converted = dynamic_cast<Value<TypeOfValue>*>(evaluated.get())) {
                    ret->set(ndx++, converted->get(0));
                }
                else {
                    throw InvalidQueryError(util::format("Invalid constant inside constant list: %1",
                                                         evaluated->description(drv->m_serializer_state)));
                }
            }
            return ret;
        }
        catch (const std::runtime_error& e) {
            throw InvalidQueryArgError(e.what());
        }
    }

    auto ret = std::make_unique<ConstantMixedList>(elements.size());
    ret->set_comparison_type(m_comp_type);
    size_t ndx = 0;
    for (auto constant : elements) {
        auto evaulated_constant = constant->visit(drv, hint);
        if (auto value = dynamic_cast<const ValueBase*>(evaulated_constant.get())) {
            REALM_ASSERT_EX(value->size() == 1, value->size());
            ret->set(ndx++, value->get(0));
        }
        else {
            throw InvalidQueryError("Invalid constant inside constant list");
        }
    }
    return ret;
}

PathElem::PathElem(const PathElem& other)
    : id(other.id)
    , index(other.index)
{
    index.use_buffer(buffer);
}

PathElem& PathElem::operator=(const PathElem& other)
{
    id = other.id;
    index = other.index;
    index.use_buffer(buffer);

    return *this;
}

LinkChain PathNode::visit(ParserDriver* drv, util::Optional<ExpressionComparisonType> comp_type)
{
    LinkChain link_chain(drv->m_base_table, comp_type);
    auto end = path_elems.end() - 1;
    for (auto it = path_elems.begin(); it != end; ++it) {
        if (!it->index.is_null()) {
            throw InvalidQueryError("Index not supported");
        }
        std::string& raw_path_elem = it->id;
        auto path_elem = drv->translate(link_chain, raw_path_elem);
        if (path_elem.find("@links.") == 0) {
            drv->backlink(link_chain, path_elem);
            continue;
        }
        if (path_elem == "@values") {
            if (!link_chain.get_current_col().is_dictionary()) {
                throw InvalidQueryError("@values only allowed on dictionaries");
            }
            continue;
        }
        if (path_elem.empty()) {
            continue; // this element has been removed, this happens in subqueries
        }

        try {
            link_chain.link(path_elem);
        }
        // In case of exception, we have to throw InvalidQueryError
        catch (const LogicError& e) {
            throw InvalidQueryError(e.what());
        }
    }
    return link_chain;
}

DescriptorNode::~DescriptorNode() {}

DescriptorOrderingNode::~DescriptorOrderingNode() {}

std::unique_ptr<DescriptorOrdering> DescriptorOrderingNode::visit(ParserDriver* drv)
{
    auto target = drv->m_base_table;
    std::unique_ptr<DescriptorOrdering> ordering;
    for (auto cur_ordering : orderings) {
        if (!ordering)
            ordering = std::make_unique<DescriptorOrdering>();
        if (cur_ordering->get_type() == DescriptorNode::LIMIT) {
            ordering->append_limit(LimitDescriptor(cur_ordering->limit));
        }
        else {
            bool is_distinct = cur_ordering->get_type() == DescriptorNode::DISTINCT;
            std::vector<std::vector<ExtendedColumnKey>> property_columns;
            for (auto& col_names : cur_ordering->columns) {
                std::vector<ExtendedColumnKey> columns;
                LinkChain link_chain(target);
                for (size_t ndx_in_path = 0; ndx_in_path < col_names.size(); ++ndx_in_path) {
                    std::string prop_name = drv->translate(link_chain, col_names[ndx_in_path].id);
                    ColKey col_key = link_chain.get_current_table()->get_column_key(prop_name);
                    if (!col_key) {
                        throw InvalidQueryError(util::format(
                            "No property '%1' found on object type '%2' specified in '%3' clause", prop_name,
                            link_chain.get_current_table()->get_class_name(), is_distinct ? "distinct" : "sort"));
                    }
                    columns.emplace_back(col_key, col_names[ndx_in_path].index);
                    if (ndx_in_path < col_names.size() - 1) {
                        link_chain.link(col_key);
                    }
                }
                property_columns.push_back(columns);
            }

            if (is_distinct) {
                ordering->append_distinct(DistinctDescriptor(property_columns));
            }
            else {
                ordering->append_sort(SortDescriptor(property_columns, cur_ordering->ascending),
                                      SortDescriptor::MergeMode::prepend);
            }
        }
    }

    return ordering;
}

// If one of the expresions is constant, it should be right
static void verify_conditions(Subexpr* left, Subexpr* right, util::serializer::SerialisationState& state)
{
    if (dynamic_cast<ColumnListBase*>(left) && dynamic_cast<ColumnListBase*>(right)) {
        throw InvalidQueryError(
            util::format("Ordered comparison between two primitive lists is not implemented yet ('%1' and '%2')",
                         left->description(state), right->description(state)));
    }
    if (dynamic_cast<Value<TypeOfValue>*>(left) && dynamic_cast<Value<TypeOfValue>*>(right)) {
        throw InvalidQueryError(util::format("Comparison between two constants is not supported ('%1' and '%2')",
                                             left->description(state), right->description(state)));
    }
    if (auto link_column = dynamic_cast<Columns<Link>*>(left)) {
        if (link_column->has_multiple_values() && right->has_single_value() && right->get_mixed().is_null()) {
            throw InvalidQueryError(
                util::format("Cannot compare linklist ('%1') with NULL", left->description(state)));
        }
    }
}

ParserDriver::ParserDriver(TableRef t, Arguments& args, const query_parser::KeyPathMapping& mapping)
    : m_base_table(t)
    , m_args(args)
    , m_mapping(mapping)
{
    yylex_init(&m_yyscanner);
}

ParserDriver::~ParserDriver()
{
    yylex_destroy(m_yyscanner);
}

Mixed ParserDriver::get_arg_for_index(const std::string& i)
{
    REALM_ASSERT(i[0] == '$');
    size_t arg_no = size_t(strtol(i.substr(1).c_str(), nullptr, 10));
    if (m_args.is_argument_null(arg_no) || m_args.is_argument_list(arg_no)) {
        throw InvalidQueryError("Invalid index parameter");
    }
    auto type = m_args.type_for_argument(arg_no);
    switch (type) {
        case type_Int:
            return int64_t(m_args.long_for_argument(arg_no));
        case type_String:
            return m_args.string_for_argument(arg_no);
        default:
            throw InvalidQueryError("Invalid index type");
    }
}

double ParserDriver::get_arg_for_coordinate(const std::string& str)
{
    REALM_ASSERT(str[0] == '$');
    size_t arg_no = size_t(strtol(str.substr(1).c_str(), nullptr, 10));
    if (m_args.is_argument_null(arg_no)) {
        throw InvalidQueryError(util::format("NULL cannot be used in coordinate at argument '%1'", str));
    }
    if (m_args.is_argument_list(arg_no)) {
        throw InvalidQueryError(util::format("A list cannot be used in a coordinate at argument '%1'", str));
    }

    auto type = m_args.type_for_argument(arg_no);
    switch (type) {
        case type_Int:
            return double(m_args.long_for_argument(arg_no));
        case type_Double:
            return m_args.double_for_argument(arg_no);
        case type_Float:
            return double(m_args.float_for_argument(arg_no));
        default:
            throw InvalidQueryError(util::format("Invalid parameter '%1' used in coordinate at argument '%2'",
                                                 get_data_type_name(type), str));
    }
}

auto ParserDriver::cmp(const std::vector<ExpressionNode*>& values) -> std::pair<SubexprPtr, SubexprPtr>
{
    SubexprPtr left;
    SubexprPtr right;

    auto left_is_constant = values[0]->is_constant();
    auto right_is_constant = values[1]->is_constant();

    if (left_is_constant && right_is_constant) {
        throw InvalidQueryError("Cannot compare two constants");
    }

    if (right_is_constant) {
        // Take left first - it cannot be a constant
        left = values[0]->visit(this);
        right = values[1]->visit(this, left->get_type());
        verify_conditions(left.get(), right.get(), m_serializer_state);
    }
    else {
        right = values[1]->visit(this);
        if (left_is_constant) {
            left = values[0]->visit(this, right->get_type());
        }
        else {
            left = values[0]->visit(this);
        }
        verify_conditions(right.get(), left.get(), m_serializer_state);
    }
    return {std::move(left), std::move(right)};
}

auto ParserDriver::column(LinkChain& link_chain, const std::string& identifier) -> SubexprPtr
{
    auto translated_identifier = m_mapping.translate(link_chain, identifier);

    if (translated_identifier.find("@links.") == 0) {
        backlink(link_chain, translated_identifier);
        return link_chain.create_subexpr<Link>(ColKey());
    }
    if (auto col = link_chain.column(translated_identifier)) {
        return col;
    }
    throw InvalidQueryError(
        util::format("'%1' has no property '%2'", link_chain.get_current_table()->get_class_name(), identifier));
}

auto ParserDriver::dictionary_column(LinkChain& link_chain, const std::string& identifier) -> SubexprPtr
{
    auto translated_identifier = m_mapping.translate(link_chain, identifier);
    auto col_key = link_chain.get_current_table()->get_column_key(translated_identifier);
    if (col_key.is_dictionary()) {
        return link_chain.create_subexpr<Dictionary>(col_key);
    }
    return {};
}

void ParserDriver::backlink(LinkChain& link_chain, const std::string& identifier)
{
    auto table_column_pair = identifier.substr(7);
    auto dot_pos = table_column_pair.find('.');

    auto table_name = table_column_pair.substr(0, dot_pos);
    table_name = m_mapping.translate_table_name(table_name);
    auto origin_table = m_base_table->get_parent_group()->get_table(table_name);
    auto column_name = table_column_pair.substr(dot_pos + 1);
    ColKey origin_column;
    if (origin_table) {
        column_name = m_mapping.translate(origin_table, column_name);
        origin_column = origin_table->get_column_key(column_name);
    }
    if (!origin_column) {
        auto origin_table_name = Group::table_name_to_class_name(table_name);
        auto current_table_name = link_chain.get_current_table()->get_class_name();
        throw InvalidQueryError(util::format("No property '%1' found in type '%2' which links to type '%3'",
                                             column_name, origin_table_name, current_table_name));
    }
    link_chain.backlink(*origin_table, origin_column);
}

std::string ParserDriver::translate(const LinkChain& link_chain, const std::string& identifier)
{
    return m_mapping.translate(link_chain, identifier);
}

int ParserDriver::parse(const std::string& str)
{
    // std::cout << str << std::endl;
    parse_buffer.append(str);
    parse_buffer.append("\0\0", 2); // Flex requires 2 terminating zeroes
    scan_begin(m_yyscanner, trace_scanning);
    yy::parser parse(*this, m_yyscanner);
    parse.set_debug_level(trace_parsing);
    int res = parse();
    if (parse_error) {
        throw SyntaxError(util::format("Invalid predicate: '%1': %2", str, error_string));
    }
    return res;
}

void parse(const std::string& str)
{
    ParserDriver driver;
    driver.parse(str);
}

std::string check_escapes(const char* str)
{
    std::string ret;
    const char* p = strchr(str, '\\');
    while (p) {
        ret += std::string(str, p);
        p++;
        if (*p == ' ') {
            ret += ' ';
        }
        else if (*p == 't') {
            ret += '\t';
        }
        else if (*p == 'r') {
            ret += '\r';
        }
        else if (*p == 'n') {
            ret += '\n';
        }
        str = p + 1;
        p = strchr(str, '\\');
    }
    return ret + std::string(str);
}

} // namespace query_parser

Query Table::query(const std::string& query_string, const std::vector<MixedArguments::Arg>& arguments) const
{
    MixedArguments args(arguments);
    return query(query_string, args, {});
}

Query Table::query(const std::string& query_string, const std::vector<Mixed>& arguments) const
{
    MixedArguments args(arguments);
    return query(query_string, args, {});
}

Query Table::query(const std::string& query_string, const std::vector<Mixed>& arguments,
                   const query_parser::KeyPathMapping& mapping) const
{
    MixedArguments args(arguments);
    return query(query_string, args, mapping);
}

Query Table::query(const std::string& query_string, const std::vector<MixedArguments::Arg>& arguments,
                   const query_parser::KeyPathMapping& mapping) const
{
    MixedArguments args(arguments);
    return query(query_string, args, mapping);
}

Query Table::query(const std::string& query_string, query_parser::Arguments& args,
                   const query_parser::KeyPathMapping& mapping) const
{
    ParserDriver driver(m_own_ref, args, mapping);
    driver.parse(query_string);
    driver.result->canonicalize();
    return driver.result->visit(&driver).set_ordering(driver.ordering->visit(&driver));
}

std::unique_ptr<Subexpr> LinkChain::column(const std::string& col)
{
    auto col_key = m_current_table->get_column_key(col);
    if (!col_key) {
        return nullptr;
    }
    size_t list_count = 0;
    for (ColKey link_key : m_link_cols) {
        if (link_key.get_type() == col_type_LinkList || link_key.get_type() == col_type_BackLink) {
            list_count++;
        }
    }

    auto col_type{col_key.get_type()};
    if (Table::is_link_type(col_type)) {
        add(col_key);
        return create_subexpr<Link>(col_key);
    }

    if (col_key.is_dictionary()) {
        return create_subexpr<Dictionary>(col_key);
    }
    else if (col_key.is_set()) {
        switch (col_type) {
            case col_type_Int:
                return create_subexpr<Set<Int>>(col_key);
            case col_type_Bool:
                return create_subexpr<Set<Bool>>(col_key);
            case col_type_String:
                return create_subexpr<Set<String>>(col_key);
            case col_type_Binary:
                return create_subexpr<Set<Binary>>(col_key);
            case col_type_Float:
                return create_subexpr<Set<Float>>(col_key);
            case col_type_Double:
                return create_subexpr<Set<Double>>(col_key);
            case col_type_Timestamp:
                return create_subexpr<Set<Timestamp>>(col_key);
            case col_type_Decimal:
                return create_subexpr<Set<Decimal>>(col_key);
            case col_type_UUID:
                return create_subexpr<Set<UUID>>(col_key);
            case col_type_ObjectId:
                return create_subexpr<Set<ObjectId>>(col_key);
            case col_type_Mixed:
                return create_subexpr<Set<Mixed>>(col_key);
            default:
                break;
        }
    }
    else if (col_key.is_list()) {
        switch (col_type) {
            case col_type_Int:
                return create_subexpr<Lst<Int>>(col_key);
            case col_type_Bool:
                return create_subexpr<Lst<Bool>>(col_key);
            case col_type_String:
                return create_subexpr<Lst<String>>(col_key);
            case col_type_Binary:
                return create_subexpr<Lst<Binary>>(col_key);
            case col_type_Float:
                return create_subexpr<Lst<Float>>(col_key);
            case col_type_Double:
                return create_subexpr<Lst<Double>>(col_key);
            case col_type_Timestamp:
                return create_subexpr<Lst<Timestamp>>(col_key);
            case col_type_Decimal:
                return create_subexpr<Lst<Decimal>>(col_key);
            case col_type_UUID:
                return create_subexpr<Lst<UUID>>(col_key);
            case col_type_ObjectId:
                return create_subexpr<Lst<ObjectId>>(col_key);
            case col_type_Mixed:
                return create_subexpr<Lst<Mixed>>(col_key);
            default:
                break;
        }
    }
    else {
        if (m_comparison_type && list_count == 0) {
            throw InvalidQueryError(util::format("The keypath following '%1' must contain a list",
                                                 expression_cmp_type_to_str(m_comparison_type)));
        }

        switch (col_type) {
            case col_type_Int:
                return create_subexpr<Int>(col_key);
            case col_type_Bool:
                return create_subexpr<Bool>(col_key);
            case col_type_String:
                return create_subexpr<String>(col_key);
            case col_type_Binary:
                return create_subexpr<Binary>(col_key);
            case col_type_Float:
                return create_subexpr<Float>(col_key);
            case col_type_Double:
                return create_subexpr<Double>(col_key);
            case col_type_Timestamp:
                return create_subexpr<Timestamp>(col_key);
            case col_type_Decimal:
                return create_subexpr<Decimal>(col_key);
            case col_type_UUID:
                return create_subexpr<UUID>(col_key);
            case col_type_ObjectId:
                return create_subexpr<ObjectId>(col_key);
            case col_type_Mixed:
                return create_subexpr<Mixed>(col_key);
            default:
                break;
        }
    }
    REALM_UNREACHABLE();
    return nullptr;
}

std::unique_ptr<Subexpr> LinkChain::subquery(Query subquery)
{
    REALM_ASSERT(m_link_cols.size() > 0);
    auto col_key = m_link_cols.back();
    return std::make_unique<SubQueryCount>(subquery, Columns<Link>(col_key, m_base_table, m_link_cols).link_map());
}

template <class T>
SubQuery<T> column(const Table& origin, ColKey origin_col_key, Query subquery)
{
    static_assert(std::is_same<T, BackLink>::value, "A subquery must involve a link list or backlink column");
    return SubQuery<T>(column<T>(origin, origin_col_key), std::move(subquery));
}

} // namespace realm
