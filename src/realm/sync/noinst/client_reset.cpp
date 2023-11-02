///////////////////////////////////////////////////////////////////////////
//
// Copyright 2021 Realm Inc.
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

#include <realm/transaction.hpp>
#include <realm/dictionary.hpp>
#include <realm/object_converter.hpp>
#include <realm/table_view.hpp>
#include <realm/set.hpp>

#include <realm/sync/history.hpp>
#include <realm/sync/changeset_parser.hpp>
#include <realm/sync/instruction_applier.hpp>
#include <realm/sync/noinst/client_history_impl.hpp>
#include <realm/sync/noinst/client_reset.hpp>
#include <realm/sync/noinst/client_reset_recovery.hpp>
#include <realm/sync/subscriptions.hpp>

#include <realm/util/compression.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

using namespace realm;
using namespace _impl;
using namespace sync;

namespace realm {

std::ostream& operator<<(std::ostream& os, const ClientResyncMode& mode)
{
    switch (mode) {
        case ClientResyncMode::Manual:
            os << "Manual";
            break;
        case ClientResyncMode::DiscardLocal:
            os << "DiscardLocal";
            break;
        case ClientResyncMode::Recover:
            os << "Recover";
            break;
        case ClientResyncMode::RecoverOrDiscard:
            os << "RecoverOrDiscard";
            break;
    }
    return os;
}

} // namespace realm

namespace realm::_impl::client_reset {

static inline bool should_skip_table(const Transaction& group, TableKey key)
{
    return !group.table_is_public(key);
}

void transfer_group(const Transaction& group_src, Transaction& group_dst, util::Logger& logger,
                    bool allow_schema_additions)
{
    logger.debug("transfer_group, src size = %1, dst size = %2, allow_schema_additions = %3", group_src.size(),
                 group_dst.size(), allow_schema_additions);

    // Turn off the sync history tracking during state transfer since it will be thrown
    // away immediately after anyways. This reduces the memory footprint of a client reset.
    ClientReplication* client_repl = dynamic_cast<ClientReplication*>(group_dst.get_replication());
    REALM_ASSERT_RELEASE(client_repl);
    TempShortCircuitReplication sync_history_guard(*client_repl);

    // Find all tables in dst that should be removed.
    std::set<std::string> tables_to_remove;
    for (auto table_key : group_dst.get_table_keys()) {
        if (should_skip_table(group_dst, table_key))
            continue;
        StringData table_name = group_dst.get_table_name(table_key);
        logger.debug("key = %1, table_name = %2", table_key.value, table_name);
        ConstTableRef table_src = group_src.get_table(table_name);
        if (!table_src) {
            logger.debug("Table '%1' will be removed", table_name);
            tables_to_remove.insert(table_name);
            continue;
        }
        // Check whether the table type is the same.
        TableRef table_dst = group_dst.get_table(table_key);
        auto pk_col_src = table_src->get_primary_key_column();
        auto pk_col_dst = table_dst->get_primary_key_column();
        bool has_pk_src = bool(pk_col_src);
        bool has_pk_dst = bool(pk_col_dst);
        if (has_pk_src != has_pk_dst) {
            throw ClientResetFailed(util::format("Client reset requires a primary key column in %1 table '%2'",
                                                 (has_pk_src ? "dest" : "source"), table_name));
        }
        if (!has_pk_src)
            continue;

        // Now the tables both have primary keys. Check type.
        if (pk_col_src.get_type() != pk_col_dst.get_type()) {
            throw ClientResetFailed(
                util::format("Client reset found incompatible primary key types (%1 vs %2) on '%3'",
                             pk_col_src.get_type(), pk_col_dst.get_type(), table_name));
        }
        // Check collection type, nullability etc. but having an index doesn't matter;
        ColumnAttrMask pk_col_src_attr = pk_col_src.get_attrs();
        ColumnAttrMask pk_col_dst_attr = pk_col_dst.get_attrs();
        pk_col_src_attr.reset(ColumnAttr::col_attr_Indexed);
        pk_col_dst_attr.reset(ColumnAttr::col_attr_Indexed);
        if (pk_col_src_attr != pk_col_dst_attr) {
            throw ClientResetFailed(
                util::format("Client reset found incompatible primary key attributes (%1 vs %2) on '%3'",
                             pk_col_src.value, pk_col_dst.value, table_name));
        }
        // Check name.
        StringData pk_col_name_src = table_src->get_column_name(pk_col_src);
        StringData pk_col_name_dst = table_dst->get_column_name(pk_col_dst);
        if (pk_col_name_src != pk_col_name_dst) {
            throw ClientResetFailed(
                util::format("Client reset requires equal pk column names but '%1' != '%2' on '%3'", pk_col_name_src,
                             pk_col_name_dst, table_name));
        }
        // The table survives.
        logger.debug("Table '%1' will remain", table_name);
    }

    // If there have been any tables marked for removal stop.
    // We consider two possible options for recovery:
    // 1: Remove the tables. But this will generate destructive schema
    //    schema changes that the local Realm cannot advance through.
    //    Since this action will fail down the line anyway, give up now.
    // 2: Keep the tables locally and ignore them. But the local app schema
    //    still has these classes and trying to modify anything in them will
    //    create sync instructions on tables that sync doesn't know about.
    // As an exception in recovery mode, we assume that the corresponding
    // additive schema changes will be part of the recovery upload. If they
    // are present, then the server can choose to allow them (if in dev mode).
    // If they are not present, then the server will emit an error the next time
    // a value is set on the unknown property.
    if (!allow_schema_additions && !tables_to_remove.empty()) {
        std::string names_list;
        for (const std::string& table_name : tables_to_remove) {
            names_list += Group::table_name_to_class_name(table_name);
            names_list += ", ";
        }
        if (names_list.size() > 2) {
            // remove the final ", "
            names_list = names_list.substr(0, names_list.size() - 2);
        }
        throw ClientResetFailed(
            util::format("Client reset cannot recover when classes have been removed: {%1}", names_list));
    }

    // Create new tables in dst if needed.
    for (auto table_key : group_src.get_table_keys()) {
        if (should_skip_table(group_src, table_key))
            continue;
        ConstTableRef table_src = group_src.get_table(table_key);
        StringData table_name = table_src->get_name();
        auto pk_col_src = table_src->get_primary_key_column();
        TableRef table_dst = group_dst.get_table(table_name);
        if (!table_dst) {
            // Create the table.
            if (table_src->is_embedded()) {
                REALM_ASSERT(!pk_col_src);
                group_dst.add_table(table_name, Table::Type::Embedded);
            }
            else {
                REALM_ASSERT(pk_col_src); // a sync table will have a pk
                auto pk_col_src = table_src->get_primary_key_column();
                DataType pk_type = DataType(pk_col_src.get_type());
                StringData pk_col_name = table_src->get_column_name(pk_col_src);
                group_dst.add_table_with_primary_key(table_name, pk_type, pk_col_name, pk_col_src.is_nullable(),
                                                     table_src->get_table_type());
            }
        }
    }

    // Now the class tables are identical.
    size_t num_tables;
    {
        size_t num_tables_src = 0;
        for (auto table_key : group_src.get_table_keys()) {
            if (!should_skip_table(group_src, table_key))
                ++num_tables_src;
        }
        size_t num_tables_dst = 0;
        for (auto table_key : group_dst.get_table_keys()) {
            if (!should_skip_table(group_dst, table_key))
                ++num_tables_dst;
        }
        REALM_ASSERT_EX(allow_schema_additions || num_tables_src == num_tables_dst, num_tables_src, num_tables_dst);
        num_tables = num_tables_src;
    }
    logger.debug("The number of tables is %1", num_tables);

    // Remove columns in dst if they are absent in src.
    for (auto table_key : group_src.get_table_keys()) {
        if (should_skip_table(group_src, table_key))
            continue;
        ConstTableRef table_src = group_src.get_table(table_key);
        StringData table_name = table_src->get_name();
        TableRef table_dst = group_dst.get_table(table_name);
        REALM_ASSERT(table_dst);
        std::vector<std::string> columns_to_remove;
        for (ColKey col_key : table_dst->get_column_keys()) {
            StringData col_name = table_dst->get_column_name(col_key);
            ColKey col_key_src = table_src->get_column_key(col_name);
            if (!col_key_src) {
                columns_to_remove.push_back(col_name);
                continue;
            }
        }
        if (!allow_schema_additions && !columns_to_remove.empty()) {
            std::string columns_list;
            for (const std::string& col_name : columns_to_remove) {
                columns_list += col_name;
                columns_list += ", ";
            }
            throw ClientResetFailed(
                util::format("Client reset cannot recover when columns have been removed from '%1': {%2}", table_name,
                             columns_list));
        }
    }

    // Add columns in dst if present in src and absent in dst.
    for (auto table_key : group_src.get_table_keys()) {
        if (should_skip_table(group_src, table_key))
            continue;
        ConstTableRef table_src = group_src.get_table(table_key);
        StringData table_name = table_src->get_name();
        TableRef table_dst = group_dst.get_table(table_name);
        REALM_ASSERT(table_dst);
        for (ColKey col_key : table_src->get_column_keys()) {
            StringData col_name = table_src->get_column_name(col_key);
            ColKey col_key_dst = table_dst->get_column_key(col_name);
            if (!col_key_dst) {
                DataType col_type = table_src->get_column_type(col_key);
                bool nullable = col_key.is_nullable();
                auto search_index_type = table_src->search_index_type(col_key);
                logger.trace("Create column, table = %1, column name = %2, "
                             " type = %3, nullable = %4, search_index = %5",
                             table_name, col_name, col_key.get_type(), nullable, search_index_type);
                ColKey col_key_dst;
                if (Table::is_link_type(col_key.get_type())) {
                    ConstTableRef target_src = table_src->get_link_target(col_key);
                    TableRef target_dst = group_dst.get_table(target_src->get_name());
                    if (col_key.is_list()) {
                        col_key_dst = table_dst->add_column_list(*target_dst, col_name);
                    }
                    else if (col_key.is_set()) {
                        col_key_dst = table_dst->add_column_set(*target_dst, col_name);
                    }
                    else if (col_key.is_dictionary()) {
                        DataType key_type = table_src->get_dictionary_key_type(col_key);
                        col_key_dst = table_dst->add_column_dictionary(*target_dst, col_name, key_type);
                    }
                    else {
                        REALM_ASSERT(!col_key.is_collection());
                        col_key_dst = table_dst->add_column(*target_dst, col_name);
                    }
                }
                else if (col_key.is_list()) {
                    col_key_dst = table_dst->add_column_list(col_type, col_name, nullable);
                }
                else if (col_key.is_set()) {
                    col_key_dst = table_dst->add_column_set(col_type, col_name, nullable);
                }
                else if (col_key.is_dictionary()) {
                    DataType key_type = table_src->get_dictionary_key_type(col_key);
                    col_key_dst = table_dst->add_column_dictionary(col_type, col_name, nullable, key_type);
                }
                else {
                    REALM_ASSERT(!col_key.is_collection());
                    col_key_dst = table_dst->add_column(col_type, col_name, nullable);
                }

                if (search_index_type != IndexType::None)
                    table_dst->add_search_index(col_key_dst, search_index_type);
            }
            else {
                // column preexists in dest, make sure the types match
                if (col_key.get_type() != col_key_dst.get_type()) {
                    throw ClientResetFailed(util::format(
                        "Incompatable column type change detected during client reset for '%1.%2' (%3 vs %4)",
                        table_name, col_name, col_key.get_type(), col_key_dst.get_type()));
                }
                ColumnAttrMask src_col_attrs = col_key.get_attrs();
                ColumnAttrMask dst_col_attrs = col_key_dst.get_attrs();
                src_col_attrs.reset(ColumnAttr::col_attr_Indexed);
                dst_col_attrs.reset(ColumnAttr::col_attr_Indexed);
                // make sure the attributes such as collection type, nullability etc. match
                // but index equality doesn't matter here.
                if (src_col_attrs != dst_col_attrs) {
                    throw ClientResetFailed(util::format(
                        "Incompatable column attribute change detected during client reset for '%1.%2' (%3 vs %4)",
                        table_name, col_name, col_key.value, col_key_dst.value));
                }
            }
        }
    }

    // Now the schemas are identical.

    // Remove objects in dst that are absent in src.
    for (auto table_key : group_src.get_table_keys()) {
        if (should_skip_table(group_src, table_key))
            continue;
        auto table_src = group_src.get_table(table_key);
        // There are no primary keys in embedded tables but this is ok, because
        // embedded objects are tied to the lifetime of top level objects.
        if (table_src->is_embedded())
            continue;
        StringData table_name = table_src->get_name();
        logger.debug("Removing objects in '%1'", table_name);
        auto table_dst = group_dst.get_table(table_name);

        auto pk_col = table_dst->get_primary_key_column();
        REALM_ASSERT_DEBUG(pk_col); // sync realms always have a pk
        std::vector<std::pair<Mixed, ObjKey>> objects_to_remove;
        for (auto obj : *table_dst) {
            auto pk = obj.get_any(pk_col);
            if (!table_src->find_primary_key(pk)) {
                objects_to_remove.emplace_back(pk, obj.get_key());
            }
        }
        for (auto& pair : objects_to_remove) {
            logger.debug("  removing '%1'", pair.first);
            table_dst->remove_object(pair.second);
        }
    }

    // We must re-create any missing objects that are absent in dst before trying to copy
    // their properties because creating them may re-create any dangling links which would
    // otherwise cause inconsistencies when re-creating lists of links.
    for (auto table_key : group_src.get_table_keys()) {
        ConstTableRef table_src = group_src.get_table(table_key);
        auto table_name = table_src->get_name();
        if (should_skip_table(group_src, table_key) || table_src->is_embedded())
            continue;
        TableRef table_dst = group_dst.get_table(table_name);
        auto pk_col = table_src->get_primary_key_column();
        REALM_ASSERT(pk_col);
        logger.debug("Creating missing objects for table '%1', number of rows = %2, "
                     "primary_key_col = %3, primary_key_type = %4",
                     table_name, table_src->size(), pk_col.get_index().val, pk_col.get_type());
        for (const Obj& src : *table_src) {
            bool created = false;
            table_dst->create_object_with_primary_key(src.get_primary_key(), &created);
            if (created) {
                logger.debug("   created %1", src.get_primary_key());
            }
        }
    }

    converters::EmbeddedObjectConverter embedded_tracker;
    // Now src and dst have identical schemas and all the top level objects are created.
    // What is left to do is to diff all properties of the existing objects.
    // Embedded objects are created on the fly.
    for (auto table_key : group_src.get_table_keys()) {
        if (should_skip_table(group_src, table_key))
            continue;
        ConstTableRef table_src = group_src.get_table(table_key);
        // Embedded objects don't have a primary key, so they are handled
        // as a special case when they are encountered as a link value.
        if (table_src->is_embedded())
            continue;
        StringData table_name = table_src->get_name();
        TableRef table_dst = group_dst.get_table(table_name);
        REALM_ASSERT_EX(allow_schema_additions || table_src->get_column_count() == table_dst->get_column_count(),
                        allow_schema_additions, table_src->get_column_count(), table_dst->get_column_count());
        auto pk_col = table_src->get_primary_key_column();
        REALM_ASSERT(pk_col);
        logger.debug("Updating values for table '%1', number of rows = %2, "
                     "number of columns = %3, primary_key_col = %4, "
                     "primary_key_type = %5",
                     table_name, table_src->size(), table_src->get_column_count(), pk_col.get_index().val,
                     pk_col.get_type());

        converters::InterRealmObjectConverter converter(table_src, table_dst, &embedded_tracker);

        for (const Obj& src : *table_src) {
            auto src_pk = src.get_primary_key();
            // create the object - it should have been created above.
            auto dst = table_dst->get_object_with_primary_key(src_pk);
            REALM_ASSERT(dst);

            bool updated = false;
            converter.copy(src, dst, &updated);
            if (updated) {
                logger.debug("  updating %1", src_pk);
            }
        }
        embedded_tracker.process_pending();
    }
}

// A table without a "class_" prefix will not generate sync instructions.
constexpr static std::string_view s_meta_reset_table_name("client_reset_metadata");
constexpr static std::string_view s_pk_col_name("id");
constexpr static std::string_view s_version_column_name("version");
constexpr static std::string_view s_timestamp_col_name("event_time");
constexpr static std::string_view s_reset_type_col_name("type_of_reset");
constexpr int64_t metadata_version = 1;

void remove_pending_client_resets(TransactionRef wt)
{
    REALM_ASSERT(wt);
    if (auto table = wt->get_table(s_meta_reset_table_name)) {
        if (table->size()) {
            table->clear();
        }
    }
}

util::Optional<PendingReset> has_pending_reset(TransactionRef rt)
{
    REALM_ASSERT(rt);
    ConstTableRef table = rt->get_table(s_meta_reset_table_name);
    if (!table || table->size() == 0) {
        return util::none;
    }
    ColKey timestamp_col = table->get_column_key(s_timestamp_col_name);
    ColKey type_col = table->get_column_key(s_reset_type_col_name);
    ColKey version_col = table->get_column_key(s_version_column_name);
    REALM_ASSERT(timestamp_col);
    REALM_ASSERT(type_col);
    REALM_ASSERT(version_col);
    if (table->size() > 1) {
        // this may happen if a future version of this code changes the format and expectations around reset metadata.
        throw ClientResetFailed(
            util::format("Previous client resets detected (%1) but only one is expected.", table->size()));
    }
    Obj first = *table->begin();
    REALM_ASSERT(first);
    PendingReset pending;
    int64_t version = first.get<int64_t>(version_col);
    pending.time = first.get<Timestamp>(timestamp_col);
    if (version > metadata_version) {
        throw ClientResetFailed(util::format("Unsupported client reset metadata version: %1 vs %2, from %3", version,
                                             metadata_version, pending.time));
    }
    int64_t type = first.get<int64_t>(type_col);
    if (type == 0) {
        pending.type = ClientResyncMode::DiscardLocal;
    }
    else if (type == 1) {
        pending.type = ClientResyncMode::Recover;
    }
    else {
        throw ClientResetFailed(
            util::format("Unsupported client reset metadata type: %1 from %2", type, pending.time));
    }
    return pending;
}

void track_reset(TransactionRef wt, ClientResyncMode mode)
{
    REALM_ASSERT(wt);
    REALM_ASSERT(mode != ClientResyncMode::Manual);
    TableRef table = wt->get_table(s_meta_reset_table_name);
    ColKey version_col, timestamp_col, type_col;
    if (!table) {
        table = wt->add_table_with_primary_key(s_meta_reset_table_name, type_ObjectId, s_pk_col_name);
        REALM_ASSERT(table);
        version_col = table->add_column(type_Int, s_version_column_name);
        timestamp_col = table->add_column(type_Timestamp, s_timestamp_col_name);
        type_col = table->add_column(type_Int, s_reset_type_col_name);
    }
    else {
        version_col = table->get_column_key(s_version_column_name);
        timestamp_col = table->get_column_key(s_timestamp_col_name);
        type_col = table->get_column_key(s_reset_type_col_name);
    }
    REALM_ASSERT(version_col);
    REALM_ASSERT(timestamp_col);
    REALM_ASSERT(type_col);
    int64_t mode_val = 0; // Discard
    if (mode == ClientResyncMode::Recover || mode == ClientResyncMode::RecoverOrDiscard) {
        mode_val = 1; // Recover
    }

    if (table->size() > 1) {
        // this may happen if a future version of this code changes the format and expectations around reset metadata.
        throw ClientResetFailed(
            util::format("Previous client resets detected (%1) but only one is expected.", table->size()));
    }
    table->create_object_with_primary_key(ObjectId::gen(),
                                          {{version_col, metadata_version},
                                           {timestamp_col, Timestamp(std::chrono::system_clock::now())},
                                           {type_col, mode_val}});
}

static ClientResyncMode reset_precheck_guard(TransactionRef wt, ClientResyncMode mode, bool recovery_is_allowed,
                                             util::Logger& logger)
{
    REALM_ASSERT(wt);
    if (auto previous_reset = has_pending_reset(wt)) {
        logger.info("A previous reset was detected of type: '%1' at: %2", previous_reset->type, previous_reset->time);
        switch (previous_reset->type) {
            case ClientResyncMode::Manual:
                REALM_UNREACHABLE();
            case ClientResyncMode::DiscardLocal:
                throw ClientResetFailed(util::format("A previous '%1' mode reset from %2 did not succeed, "
                                                     "giving up on '%3' mode to prevent a cycle",
                                                     previous_reset->type, previous_reset->time, mode));
            case ClientResyncMode::Recover:
                switch (mode) {
                    case ClientResyncMode::Recover:
                        throw ClientResetFailed(util::format("A previous '%1' mode reset from %2 did not succeed, "
                                                             "giving up on '%3' mode to prevent a cycle",
                                                             previous_reset->type, previous_reset->time, mode));
                    case ClientResyncMode::RecoverOrDiscard:
                        mode = ClientResyncMode::DiscardLocal;
                        logger.info("A previous '%1' mode reset from %2 downgrades this mode ('%3') to DiscardLocal",
                                    previous_reset->type, previous_reset->time, mode);
                        remove_pending_client_resets(wt);
                        break;
                    case ClientResyncMode::DiscardLocal:
                        remove_pending_client_resets(wt);
                        // previous mode Recover and this mode is Discard, this is not a cycle yet
                        break;
                    case ClientResyncMode::Manual:
                        REALM_UNREACHABLE();
                }
                break;
            case ClientResyncMode::RecoverOrDiscard:
                throw ClientResetFailed(util::format("Unexpected previous '%1' mode reset from %2 did not "
                                                     "succeed, giving up on '%3' mode to prevent a cycle",
                                                     previous_reset->type, previous_reset->time, mode));
        }
    }
    if (!recovery_is_allowed) {
        if (mode == ClientResyncMode::Recover) {
            throw ClientResetFailed(
                "Client reset mode is set to 'Recover' but the server does not allow recovery for this client");
        }
        else if (mode == ClientResyncMode::RecoverOrDiscard) {
            logger.info("Client reset in 'RecoverOrDiscard' is choosing 'DiscardLocal' because the server does not "
                        "permit recovery for this client");
            mode = ClientResyncMode::DiscardLocal;
        }
    }
    track_reset(wt, mode);
    return mode;
}

LocalVersionIDs perform_client_reset_diff(DBRef db_local, DBRef db_remote, sync::SaltedFileIdent client_file_ident,
                                          util::Logger& logger, ClientResyncMode mode, bool recovery_is_allowed,
                                          bool* did_recover_out, sync::SubscriptionStore* sub_store,
                                          util::UniqueFunction<void(int64_t)> on_flx_version_complete)
{
    REALM_ASSERT(db_local);
    REALM_ASSERT(db_remote);
    logger.info("Client reset, path_local = %1, "
                "client_file_ident.ident = %2, "
                "client_file_ident.salt = %3,"
                "remote = %4, mode = %5, recovery_is_allowed = %6",
                db_local->get_path(), client_file_ident.ident, client_file_ident.salt, db_remote->get_path(), mode,
                recovery_is_allowed);

    auto remake_active_subscription = [&]() {
        if (!sub_store) {
            return;
        }
        auto subs = sub_store->get_active();
        int64_t before_version = subs.version();
        auto mut_subs = subs.make_mutable_copy();
        mut_subs.update_state(sync::SubscriptionSet::State::Complete);
        auto sub = std::move(mut_subs).commit();
        if (on_flx_version_complete) {
            on_flx_version_complete(sub.version());
        }
        logger.info("Recreated the active subscription set in the complete state (%1 -> %2)", before_version,
                    sub.version());
    };

    auto frozen_pre_local_state = db_local->start_frozen();
    auto wt_local = db_local->start_write();
    auto history_local = dynamic_cast<ClientHistory*>(wt_local->get_replication()->_get_history_write());
    REALM_ASSERT(history_local);
    VersionID old_version_local = wt_local->get_version_of_current_transaction();
    wt_local->get_history()->ensure_updated(old_version_local.version);
    SaltedFileIdent orig_file_ident;
    {
        sync::version_type old_version_unused;
        SyncProgress old_progress_unused;
        history_local->get_status(old_version_unused, orig_file_ident, old_progress_unused);
    }
    std::vector<ClientHistory::LocalChange> local_changes;

    mode = reset_precheck_guard(wt_local, mode, recovery_is_allowed, logger);
    bool recover_local_changes = (mode == ClientResyncMode::Recover || mode == ClientResyncMode::RecoverOrDiscard);

    if (recover_local_changes) {
        local_changes = history_local->get_local_changes(wt_local->get_version());
        logger.info("Local changesets to recover: %1", local_changes.size());
    }

    sync::SaltedVersion fresh_server_version = {0, 0};
    auto wt_remote = db_remote->start_write();
    auto history_remote = dynamic_cast<ClientHistory*>(wt_remote->get_replication()->_get_history_write());
    REALM_ASSERT(history_remote);

    SyncProgress remote_progress;
    {
        sync::version_type remote_version_unused;
        SaltedFileIdent remote_ident_unused;
        history_remote->get_status(remote_version_unused, remote_ident_unused, remote_progress);
    }
    fresh_server_version = remote_progress.latest_server_version;
    BinaryData recovered_changeset;

    // FLX with recovery has to be done in multiple commits, which is significantly different than other modes
    if (recover_local_changes && sub_store) {
        // In FLX recovery, save a copy of the pending subscriptions for later. This
        // needs to be done before they are wiped out by remake_active_subscription()
        std::vector<SubscriptionSet> pending_subscriptions = sub_store->get_pending_subscriptions();
        // transform the local Realm such that all public tables become identical to the remote Realm
        transfer_group(*wt_remote, *wt_local, logger, recover_local_changes);
        // now that the state of the fresh and local Realms are identical,
        // reset the local sync history.
        // Note that we do not set the new file ident yet! This is done in the last commit.
        history_local->set_client_reset_adjustments(wt_local->get_version(), orig_file_ident, fresh_server_version,
                                                    recovered_changeset);
        // The local Realm is committed. There are no changes to the remote Realm.
        wt_remote->rollback_and_continue_as_read();
        wt_local->commit_and_continue_as_read();
        // Make a copy of the active subscription set and mark it as
        // complete. This will cause all other subscription sets to become superceded.
        remake_active_subscription();
        // Apply local changes interleaved with pending subscriptions in separate commits
        // as needed. This has the consequence that there may be extra notifications along
        // the way to the final state, but since separate commits are necessary, this is
        // unavoidable.
        wt_local = db_local->start_write();
        RecoverLocalChangesetsHandler handler{*wt_local, *frozen_pre_local_state, logger,
                                              db_local->get_replication()};
        handler.process_changesets(local_changes, std::move(pending_subscriptions)); // throws on error
        // The new file ident is set as part of the final commit. This is to ensure that if
        // there are any exceptions during recovery, or the process is killed for some
        // reason, the client reset cycle detection will catch this and we will not attempt
        // to recover again. If we had set the ident in the first commit, a Realm which was
        // partially recovered, but interrupted may continue sync the next time it is
        // opened with only partially recovered state while having lost the history of any
        // offline modifications.
        history_local->set_client_file_ident_in_wt(wt_local->get_version(), client_file_ident);
        wt_local->commit_and_continue_as_read();
    }
    else {
        if (recover_local_changes) {
            // In PBS recovery, the strategy is to apply all local changes to the remote realm first,
            // and then transfer the modified state all at once to the local Realm. This creates a
            // nice side effect for notifications because only the minimal state change is made.
            RecoverLocalChangesetsHandler handler{*wt_remote, *frozen_pre_local_state, logger,
                                                  db_remote->get_replication()};
            handler.process_changesets(local_changes, {}); // throws on error
            ClientReplication* client_repl = dynamic_cast<ClientReplication*>(wt_remote->get_replication());
            REALM_ASSERT_RELEASE(client_repl);
            ChangesetEncoder& encoder = client_repl->get_instruction_encoder();
            const sync::ChangesetEncoder::Buffer& buffer = encoder.buffer();
            recovered_changeset = {buffer.data(), buffer.size()};
        }

        // transform the local Realm such that all public tables become identical to the remote Realm
        transfer_group(*wt_remote, *wt_local, logger, recover_local_changes);

        // now that the state of the fresh and local Realms are identical,
        // reset the local sync history and steal the fresh Realm's ident
        history_local->set_client_reset_adjustments(wt_local->get_version(), client_file_ident, fresh_server_version,
                                                    recovered_changeset);

        // Finally, the local Realm is committed. The changes to the remote Realm are discarded.
        wt_remote->rollback_and_continue_as_read();
        wt_local->commit_and_continue_as_read();

        // If in FLX mode, make a copy of the active subscription set and mark it as
        // complete. This will cause all other subscription sets to become superceded.
        // In DiscardLocal mode, only the active subscription set is preserved, so we
        // are done.
        remake_active_subscription();
    }

    if (did_recover_out) {
        *did_recover_out = recover_local_changes;
    }
    VersionID new_version_local = wt_local->get_version_of_current_transaction();
    logger.info("perform_client_reset_diff is done, old_version.version = %1, "
                "old_version.index = %2, new_version.version = %3, "
                "new_version.index = %4",
                old_version_local.version, old_version_local.index, new_version_local.version,
                new_version_local.index);

    return LocalVersionIDs{old_version_local, new_version_local};
}

} // namespace realm::_impl::client_reset
