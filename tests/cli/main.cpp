/*
 * Copyright (c) 2018 John Jones, and contributors.
 * Copyright (c) 2023 R-Squared Labs LLC, and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <graphene/app/application.hpp>
#include <graphene/app/plugin.hpp>

#include <graphene/utilities/key_conversion.hpp>
#include <graphene/utilities/tempdir.hpp>

#include <graphene/account_history/account_history_plugin.hpp>
#include <graphene/api_helper_indexes/api_helper_indexes.hpp>
#include <graphene/custom_operations/custom_operations_plugin.hpp>
#include <graphene/egenesis/egenesis.hpp>
#include <graphene/wallet/wallet.hpp>
#include <graphene/chain/hardfork.hpp>

#include <fc/thread/thread.hpp>
#include <fc/network/http/websocket.hpp>
#include <fc/rpc/websocket_api.hpp>
#include <fc/rpc/cli.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/crypto/hex.hpp>

#include <fc/crypto/aes.hpp>

#ifdef _WIN32
   #ifndef _WIN32_WINNT
      #define _WIN32_WINNT 0x0501
   #endif
   #include <winsock2.h>
   #include <ws2tcpip.h>
#else
   #include <sys/types.h>
   #include <sys/socket.h>
   #include <netinet/in.h>
   #include <netinet/ip.h>
#endif
#include <thread>

#include <boost/filesystem/path.hpp>

#include "../common/init_unit_test_suite.hpp"
#include "../common/genesis_file_util.hpp"
#include "../common/program_options_util.hpp"
#include "../common/utils.hpp"

#ifdef _WIN32
/*****
 * Global Initialization for Windows
 * ( sets up Winsock stuf )
 */
int sockInit(void)
{
   WSADATA wsa_data;
   return WSAStartup(MAKEWORD(1,1), &wsa_data);
}
int sockQuit(void)
{
   return WSACleanup();
}
#endif

/*********************
 * Helper Methods
 *********************/

using std::exception;
using std::cerr;

#define INVOKE(test) ((struct test*)this)->test_method();

///////////
/// @brief Start the application
/// @param app_dir the temporary directory to use
/// @param server_port_number to be filled with the rpc endpoint port number
/// @returns the application object
//////////
std::shared_ptr<graphene::app::application> start_application(fc::temp_directory& app_dir, int& server_port_number) {
   auto app1 = std::make_shared<graphene::app::application>();

   app1->register_plugin<graphene::account_history::account_history_plugin>(true);
   app1->register_plugin< graphene::api_helper_indexes::api_helper_indexes>(true);
   app1->register_plugin<graphene::custom_operations::custom_operations_plugin>(true);

   auto sharable_cfg = std::make_shared<boost::program_options::variables_map>();
   auto& cfg = *sharable_cfg;
   server_port_number = fc::network::get_available_port();
   auto p2p_port = server_port_number;
   for( size_t i = 0; i < 10 && p2p_port == server_port_number; ++i )
   {
      p2p_port = fc::network::get_available_port();
   }
   BOOST_REQUIRE( p2p_port != server_port_number );
   fc::set_option( cfg, "rpc-endpoint", string("127.0.0.1:") + std::to_string(server_port_number) );
   fc::set_option( cfg, "p2p-endpoint", string("0.0.0.0:") + std::to_string(p2p_port) );
   fc::set_option( cfg, "genesis-json", create_genesis_file(app_dir) );
   fc::set_option( cfg, "seed-nodes", string("[]") );
   fc::set_option( cfg, "custom-operations-start-block", uint32_t(1) );
   app1->initialize(app_dir.path(), sharable_cfg);

   app1->startup();

   return app1;
}

///////////
/// Send a block to the db
/// @param app the application
/// @param returned_block the signed block
/// @returns true on success
///////////
bool generate_block(std::shared_ptr<graphene::app::application> app, graphene::chain::signed_block& returned_block)
{
   try {
      fc::ecc::private_key committee_key = fc::ecc::private_key::regenerate(fc::sha256::hash(string("rsquaredchp1")));
      auto db = app->chain_database();
      returned_block = db->generate_block( db->get_slot_time(1),
                                         db->get_scheduled_witness(1),
                                         committee_key,
                                         database::skip_nothing );
      return true;
   } catch (exception &e) {
      return false;
   }
}

bool generate_block(std::shared_ptr<graphene::app::application> app)
{
   graphene::chain::signed_block returned_block;
   return generate_block(app, returned_block);
}


signed_block generate_block(std::shared_ptr<graphene::app::application> app, uint32_t skip, const fc::ecc::private_key& key, int miss_blocks)
{
   // skip == ~0 will skip checks specified in database::validation_steps
   skip |= database::skip_undo_history_check;

   auto db = app->chain_database();
   auto block = db->generate_block(db->get_slot_time(miss_blocks + 1),
                                   db->get_scheduled_witness(miss_blocks + 1),
                                   key, skip);
   db->clear_pending();
   return block;
}


//////
// Generate blocks until the timestamp
//////
uint32_t generate_blocks(std::shared_ptr<graphene::app::application> app, fc::time_point_sec timestamp)
{
   fc::ecc::private_key committee_key = fc::ecc::private_key::regenerate(fc::sha256::hash(string("rsquaredchp1")));
   uint32_t skip = ~0;
   auto db = app->chain_database();

   generate_block(app);
   auto slots_to_miss = db->get_slot_at_time(timestamp);
   if( slots_to_miss <= 1 )
      return 1;
   --slots_to_miss;
   generate_block(app, skip, committee_key, slots_to_miss);
   return 2;
}


///////////
/// @brief Skip intermediate blocks, and generate a maintenance block
/// @param app the application
/// @returns true on success
///////////
bool generate_maintenance_block(std::shared_ptr<graphene::app::application> app) {
   try {
      fc::ecc::private_key committee_key = fc::ecc::private_key::regenerate(fc::sha256::hash(string("rsquaredchp1")));
      uint32_t skip = ~0;
      auto db = app->chain_database();
      auto maint_time = db->get_dynamic_global_properties().next_maintenance_time;
      auto slots_to_miss = db->get_slot_at_time(maint_time);
      db->generate_block(db->get_slot_time(slots_to_miss),
            db->get_scheduled_witness(slots_to_miss),
            committee_key,
            skip);
      return true;
   } catch (exception& e)
   {
      return false;
   }
}

///////////
/// @brief a class to make connecting to the application server easier
///////////
class client_connection
{
public:
   /////////
   // constructor
   /////////
   client_connection(
      std::shared_ptr<graphene::app::application> app,
      const fc::temp_directory& data_dir,
      const int server_port_number,
      const std::string custom_wallet_filename = "wallet.json"
   )
   {
      wallet_data.chain_id = app->chain_database()->get_chain_id();
      wallet_data.ws_server = "ws://127.0.0.1:" + std::to_string(server_port_number);
      wallet_data.ws_user = "";
      wallet_data.ws_password = "";
      websocket_connection  = websocket_client.connect( wallet_data.ws_server );

      api_connection = std::make_shared<fc::rpc::websocket_api_connection>( websocket_connection,
                                                                            GRAPHENE_MAX_NESTED_OBJECTS );

      remote_login_api = api_connection->get_remote_api< graphene::app::login_api >(1);
      BOOST_CHECK(remote_login_api->login( wallet_data.ws_user, wallet_data.ws_password ) );

      wallet_api_ptr = std::make_shared<graphene::wallet::wallet_api>(wallet_data, remote_login_api);
      wallet_filename = data_dir.path().generic_string() + "/" + custom_wallet_filename;
      wallet_api_ptr->set_wallet_filename(wallet_filename);

      wallet_api = fc::api<graphene::wallet::wallet_api>(wallet_api_ptr);

      wallet_cli = std::make_shared<fc::rpc::cli>(GRAPHENE_MAX_NESTED_OBJECTS);
      for( auto& name_formatter : wallet_api_ptr->get_result_formatters() )
         wallet_cli->format_result( name_formatter.first, name_formatter.second );
   }
   ~client_connection()
   {
      wallet_cli->stop();
   }
public:
   fc::http::websocket_client websocket_client;
   graphene::wallet::wallet_data wallet_data;
   fc::http::websocket_connection_ptr websocket_connection;
   std::shared_ptr<fc::rpc::websocket_api_connection> api_connection;
   fc::api<login_api> remote_login_api;
   std::shared_ptr<graphene::wallet::wallet_api> wallet_api_ptr;
   fc::api<graphene::wallet::wallet_api> wallet_api;
   std::shared_ptr<fc::rpc::cli> wallet_cli;
   std::string wallet_filename;
};

///////////////////////////////
// Cli Wallet Fixture
///////////////////////////////

struct cli_fixture
{
#ifdef _WIN32
   struct socket_maintainer {
      socket_maintainer() {
         sockInit();
      }
      ~socket_maintainer() {
         sockQuit();
      }
   } sock_maintainer;
#endif
   int server_port_number;
   fc::temp_directory app_dir;
   std::shared_ptr<graphene::app::application> app1;
   client_connection con;
   std::vector<std::string> rsquaredchp1_keys;

   cli_fixture() :
      server_port_number(0),
      app_dir( graphene::utilities::temp_directory_path() ),
      app1( start_application(app_dir, server_port_number) ),
      con( app1, app_dir, server_port_number ),
      rsquaredchp1_keys( {"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"} )
   {
      BOOST_TEST_MESSAGE("Setup cli_wallet::boost_fixture_test_case");

      using namespace graphene::chain;
      using namespace graphene::app;

      try
      {
         BOOST_TEST_MESSAGE("Setting wallet password");
         con.wallet_api_ptr->set_password("supersecret");
         con.wallet_api_ptr->unlock("supersecret");

         // import RSquaredCHP1 account
         BOOST_TEST_MESSAGE("Importing rsquaredchp1 key");
         BOOST_CHECK_EQUAL(rsquaredchp1_keys[0], "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3");
         BOOST_CHECK(con.wallet_api_ptr->import_key("rsquaredchp1", rsquaredchp1_keys[0]));
      } catch( fc::exception& e ) {
         edump((e.to_detail_string()));
         throw;
      }
   }

   ~cli_fixture()
   {
      BOOST_TEST_MESSAGE("Cleanup cli_wallet::boost_fixture_test_case");
   }
};

///////////////////////////////
// Tests
///////////////////////////////

////////////////
// Start a server and connect using the same calls as the CLI
////////////////
BOOST_FIXTURE_TEST_CASE( cli_connect, cli_fixture )
{
   BOOST_TEST_MESSAGE("Testing wallet connection.");
}

////////////////
// Start a server and connect using the same calls as the CLI
// Quit wallet and be sure that file was saved correctly
////////////////
BOOST_FIXTURE_TEST_CASE( cli_quit, cli_fixture )
{
   BOOST_TEST_MESSAGE("Testing wallet connection and quit command.");
   BOOST_CHECK_THROW( con.wallet_api_ptr->quit(), fc::canceled_exception );
}

BOOST_FIXTURE_TEST_CASE( cli_help_gethelp, cli_fixture )
{
   BOOST_TEST_MESSAGE("Testing help and gethelp commands.");
   auto formatters = con.wallet_api_ptr->get_result_formatters();

   string result = con.wallet_api_ptr->help();
   BOOST_CHECK( result.find("gethelp") != string::npos );
   if( formatters.find("help") != formatters.end() )
   {
      BOOST_TEST_MESSAGE("Testing formatter of help");
      string output = formatters["help"](fc::variant(result), fc::variants());
      BOOST_CHECK( output.find("gethelp") != string::npos );
   }

   result = con.wallet_api_ptr->gethelp( "transfer" );
   BOOST_CHECK( result.find("usage") != string::npos );
   if( formatters.find("gethelp") != formatters.end() )
   {
      BOOST_TEST_MESSAGE("Testing formatter of gethelp");
      string output = formatters["gethelp"](fc::variant(result), fc::variants());
      BOOST_CHECK( output.find("usage") != string::npos );
   }
}

BOOST_FIXTURE_TEST_CASE( upgrade_rsquaredchp1_account, cli_fixture )
{
   try
   {
      BOOST_TEST_MESSAGE("Upgrade RSquaredCHP1's account");

      account_object rsquaredchp1_acct_before_upgrade, rsquaredchp1_acct_after_upgrade;
      std::vector<signed_transaction> import_txs;
      signed_transaction upgrade_tx;

      BOOST_TEST_MESSAGE("Importing rsquaredchp1's balance");
      import_txs = con.wallet_api_ptr->import_balance("rsquaredchp1", rsquaredchp1_keys, true);
      rsquaredchp1_acct_before_upgrade = con.wallet_api_ptr->get_account("rsquaredchp1");

      // upgrade rsquaredchp1
      BOOST_TEST_MESSAGE("Upgrading RSquaredCHP1 to LTM");
      upgrade_tx = con.wallet_api_ptr->upgrade_account("rsquaredchp1", true);
      rsquaredchp1_acct_after_upgrade = con.wallet_api_ptr->get_account("rsquaredchp1");

      // verify that the upgrade was successful
      BOOST_CHECK_PREDICATE(
         std::not_equal_to<uint32_t>(),
         (rsquaredchp1_acct_before_upgrade.membership_expiration_date.sec_since_epoch())
         (rsquaredchp1_acct_after_upgrade.membership_expiration_date.sec_since_epoch())
      );
      BOOST_CHECK(rsquaredchp1_acct_after_upgrade.is_lifetime_member());
   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_FIXTURE_TEST_CASE( create_new_account, cli_fixture )
{
   try
   {
      INVOKE(upgrade_rsquaredchp1_account);

      // create a new account
      graphene::wallet::brain_key_info bki = con.wallet_api_ptr->suggest_brain_key();
      BOOST_CHECK(!bki.brain_priv_key.empty());
      signed_transaction create_acct_tx = con.wallet_api_ptr->create_account_with_brain_key(
         bki.brain_priv_key, "jmjatlanta", "rsquaredchp1", "rsquaredchp1", true
      );
      // save the private key for this new account in the wallet file
      BOOST_CHECK(con.wallet_api_ptr->import_key("jmjatlanta", bki.wif_priv_key));
      con.wallet_api_ptr->save_wallet_file(con.wallet_filename);

      // attempt to give jmjatlanta some rsquared
      BOOST_TEST_MESSAGE("Transferring rsquared from RSquaredCHP1 to jmjatlanta");
      signed_transaction transfer_tx = con.wallet_api_ptr->transfer(
         "rsquaredchp1", "jmjatlanta", "10000", "1.3.0", "Here are some CORE token for your new account", true
      );
   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_FIXTURE_TEST_CASE( uia_tests, cli_fixture )
{
   try
   {
      BOOST_TEST_MESSAGE("Cli UIA Tests");

      INVOKE(upgrade_rsquaredchp1_account);

      BOOST_CHECK(generate_block(app1));

      account_object rsquaredchp1_acct = con.wallet_api_ptr->get_account("rsquaredchp1");

      auto formatters = con.wallet_api_ptr->get_result_formatters();

      auto check_account_last_history = [&]( string account, string keyword ) {
         auto history = con.wallet_api_ptr->get_relative_account_history(account, 0, 1, 0);
         BOOST_REQUIRE_GT( history.size(), 0 );
         BOOST_CHECK( history[0].description.find( keyword ) != string::npos );
      };
      auto check_rsquaredchp1_last_history = [&]( string keyword ) {
         check_account_last_history( "rsquaredchp1", keyword );
      };

      check_rsquaredchp1_last_history( "account_upgrade_operation" );

      // Create new asset called BOBCOIN
      {
         BOOST_TEST_MESSAGE("Create UIA 'BOBCOIN'");
         graphene::chain::asset_options asset_ops;
         asset_ops.issuer_permissions = DEFAULT_UIA_ASSET_ISSUER_PERMISSION;
         asset_ops.flags = charge_market_fee | override_authority;
         asset_ops.max_supply = 1000000;
         asset_ops.core_exchange_rate = price(asset(2),asset(1,asset_id_type(1)));
         auto result = con.wallet_api_ptr->create_asset("rsquaredchp1", "BOBCOIN", 4, asset_ops, {}, true);
         if( formatters.find("create_asset") != formatters.end() )
         {
            BOOST_TEST_MESSAGE("Testing formatter of create_asset");
            string output = formatters["create_asset"](
                  fc::variant(result, FC_PACK_MAX_DEPTH), fc::variants());
            BOOST_CHECK( output.find("BOBCOIN") != string::npos );
         }

         BOOST_CHECK_THROW( con.wallet_api_ptr->get_asset_name("BOBCOI"), fc::exception );
         BOOST_CHECK_EQUAL( con.wallet_api_ptr->get_asset_name("BOBCOIN"), "BOBCOIN" );
         BOOST_CHECK_EQUAL( con.wallet_api_ptr->get_asset_symbol("BOBCOIN"), "BOBCOIN" );

         BOOST_CHECK_THROW( con.wallet_api_ptr->get_account_name("nath"), fc::exception );
         BOOST_CHECK_EQUAL( con.wallet_api_ptr->get_account_name("rsquaredchp1"), "rsquaredchp1" );
         BOOST_CHECK( con.wallet_api_ptr->get_account_id("rsquaredchp1") == con.wallet_api_ptr->get_account("rsquaredchp1").id );
      }
      BOOST_CHECK(generate_block(app1));

      check_rsquaredchp1_last_history( "Create User-Issue Asset" );
      check_rsquaredchp1_last_history( "BOBCOIN" );

      auto bobcoin = con.wallet_api_ptr->get_asset("BOBCOIN");

      BOOST_CHECK( con.wallet_api_ptr->get_asset_id("BOBCOIN") == bobcoin.id );

      bool balance_formatter_tested = false;
      auto check_bobcoin_balance = [&](string account, int64_t amount) {
         auto balances = con.wallet_api_ptr->list_account_balances( account );
         size_t count = 0;
         for( auto& bal : balances )
         {
            if( bal.asset_id == bobcoin.id )
            {
               ++count;
               BOOST_CHECK_EQUAL( bal.amount.value, amount );
            }
         }
         BOOST_CHECK_EQUAL(count, 1u);

         // Testing result formatter
         if( !balance_formatter_tested && formatters.find("list_account_balances") != formatters.end() )
         {
            BOOST_TEST_MESSAGE("Testing formatter of list_account_balances");
            string output = formatters["list_account_balances"](
                  fc::variant(balances, FC_PACK_MAX_DEPTH ), fc::variants());
            BOOST_CHECK( output.find("BOBCOIN") != string::npos );
            balance_formatter_tested = true;
         }
      };
      auto check_rsquaredchp1_bobcoin_balance = [&](int64_t amount) {
         check_bobcoin_balance( "rsquaredchp1", amount );
      };

      {
         // Issue asset
         BOOST_TEST_MESSAGE("Issue asset");
         con.wallet_api_ptr->issue_asset("init0", "3", "BOBCOIN", "new coin for you", true);
      }
      BOOST_CHECK(generate_block(app1));

      check_rsquaredchp1_last_history( "rsquaredchp1 issue 3 BOBCOIN to init0" );
      check_rsquaredchp1_last_history( "new coin for you" );
      check_account_last_history( "init0", "rsquaredchp1 issue 3 BOBCOIN to init0" );
      check_account_last_history( "init0", "new coin for you" );

      check_bobcoin_balance( "init0", 30000 );

      {
         // Override transfer, and test sign_memo and read_memo by the way
         BOOST_TEST_MESSAGE("Override-transfer BOBCOIN from init0");
         auto handle = con.wallet_api_ptr->begin_builder_transaction();
         override_transfer_operation op;
         op.issuer = con.wallet_api_ptr->get_account( "rsquaredchp1" ).id;
         op.from = con.wallet_api_ptr->get_account( "init0" ).id;
         op.to = con.wallet_api_ptr->get_account( "rsquaredchp1" ).id;
         op.amount = bobcoin.amount(10000);

         const auto test_bki = con.wallet_api_ptr->suggest_brain_key();
         auto test_pubkey = fc::json::to_string( test_bki.pub_key );
         test_pubkey = test_pubkey.substr( 1, test_pubkey.size() - 2 );
         idump( (test_pubkey) );
         op.memo = con.wallet_api_ptr->sign_memo( "rsquaredchp1", test_pubkey, "get back some coin" );
         idump( (op.memo) );
         con.wallet_api_ptr->add_operation_to_builder_transaction( handle, op );
         con.wallet_api_ptr->set_fees_on_builder_transaction( handle, "1.3.0" );
         con.wallet_api_ptr->sign_builder_transaction( handle, {}, true );

         auto memo = con.wallet_api_ptr->read_memo( *op.memo );
         BOOST_CHECK_EQUAL( memo, "get back some coin" );

         op.memo = con.wallet_api_ptr->sign_memo( test_pubkey, "rsquaredchp1", "another test" );
         idump( (op.memo) );
         memo = con.wallet_api_ptr->read_memo( *op.memo );
         BOOST_CHECK_EQUAL( memo, "another test" );

         BOOST_CHECK_THROW( con.wallet_api_ptr->sign_memo( "non-exist-account-or-label", "rsquaredchp1", "some text" ),
                            fc::exception );
         BOOST_CHECK_THROW( con.wallet_api_ptr->sign_memo( "rsquaredchp1", "non-exist-account-or-label", "some text" ),
                            fc::exception );
      }
      BOOST_CHECK(generate_block(app1));

      check_rsquaredchp1_last_history( "rsquaredchp1 force-transfer 1 BOBCOIN from init0 to rsquaredchp1" );
      check_rsquaredchp1_last_history( "get back some coin" );
      check_account_last_history( "init0", "rsquaredchp1 force-transfer 1 BOBCOIN from init0 to rsquaredchp1" );
      check_account_last_history( "init0", "get back some coin" );

      check_bobcoin_balance( "init0", 20000 );
      check_bobcoin_balance( "rsquaredchp1", 10000 );

      {
         // Reserve / burn asset
         BOOST_TEST_MESSAGE("Reserve/burn asset");
         con.wallet_api_ptr->reserve_asset("rsquaredchp1", "1", "BOBCOIN", true);
      }
      BOOST_CHECK(generate_block(app1));

      check_rsquaredchp1_last_history( "Reserve (burn) 1 BOBCOIN" );

      check_rsquaredchp1_bobcoin_balance( 0 );

   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

///////////////////////
// Start a server and connect using the same calls as the CLI
// Vote for two witnesses, and make sure they both stay there
// after a maintenance block
///////////////////////
BOOST_FIXTURE_TEST_CASE( cli_vote_for_2_witnesses, cli_fixture )
{
   try
   {
      BOOST_TEST_MESSAGE("Cli Vote Test for 2 Witnesses");

      INVOKE(create_new_account);

      // get the details for init1
      witness_object init1_obj = con.wallet_api_ptr->get_witness("init1");
      int init1_start_votes = init1_obj.total_votes;
      // Vote for a witness
      signed_transaction vote_witness1_tx = con.wallet_api_ptr->vote_for_witness("jmjatlanta", "init1", true, true);

      // generate a block to get things started
      BOOST_CHECK(generate_block(app1));
      // wait for a maintenance interval
      BOOST_CHECK(generate_maintenance_block(app1));

      // Verify that the vote is there
      init1_obj = con.wallet_api_ptr->get_witness("init1");
      witness_object init2_obj = con.wallet_api_ptr->get_witness("init2");
      int init1_middle_votes = init1_obj.total_votes;
      BOOST_CHECK(init1_middle_votes > init1_start_votes);

      // Vote for a 2nd witness
      int init2_start_votes = init2_obj.total_votes;
      signed_transaction vote_witness2_tx = con.wallet_api_ptr->vote_for_witness("jmjatlanta", "init2", true, true);

      // send another block to trigger maintenance interval
      BOOST_CHECK(generate_maintenance_block(app1));

      // Verify that both the first vote and the 2nd are there
      init2_obj = con.wallet_api_ptr->get_witness("init2");
      init1_obj = con.wallet_api_ptr->get_witness("init1");

      int init2_middle_votes = init2_obj.total_votes;
      BOOST_CHECK(init2_middle_votes > init2_start_votes);
      int init1_last_votes = init1_obj.total_votes;
      BOOST_CHECK(init1_last_votes > init1_start_votes);

      {
         auto history = con.wallet_api_ptr->get_account_history_by_operations(
                              "jmjatlanta", {2}, 0, 1); // 2 - account_update_operation
         BOOST_REQUIRE_GT( history.details.size(), 0 );
         BOOST_CHECK( history.details[0].description.find( "Update Account 'jmjatlanta'" ) != string::npos );

         // Testing result formatter
         auto formatters = con.wallet_api_ptr->get_result_formatters();
         if( formatters.find("get_account_history_by_operations") != formatters.end() )
         {
            BOOST_TEST_MESSAGE("Testing formatter of get_account_history_by_operations");
            string output = formatters["get_account_history_by_operations"](
                  fc::variant(history, FC_PACK_MAX_DEPTH), fc::variants());
            BOOST_CHECK( output.find("Update Account 'jmjatlanta'") != string::npos );
         }
      }

   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_FIXTURE_TEST_CASE( cli_get_signed_transaction_signers, cli_fixture )
{
   try
   {
      INVOKE(upgrade_rsquaredchp1_account);

      // register account and transfer funds
      const auto test_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
         "test", test_bki.pub_key, test_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );
      con.wallet_api_ptr->transfer("rsquaredchp1", "test", "1000", "1.3.0", "", true);

      // import key and save wallet
      BOOST_CHECK(con.wallet_api_ptr->import_key("test", test_bki.wif_priv_key));
      con.wallet_api_ptr->save_wallet_file(con.wallet_filename);

      // create transaction and check expected result
      auto signed_trx = con.wallet_api_ptr->transfer("test", "rsquaredchp1", "10", "1.3.0", "", true);

      const auto &test_acc = con.wallet_api_ptr->get_account("test");
      flat_set<public_key_type> expected_signers = {test_bki.pub_key};
      vector<flat_set<account_id_type> > expected_key_refs{{test_acc.id, test_acc.id}};

      auto signers = con.wallet_api_ptr->get_transaction_signers(signed_trx);
      BOOST_CHECK(signers == expected_signers);

      auto key_refs = con.wallet_api_ptr->get_key_references({expected_signers.begin(), expected_signers.end()});
      BOOST_CHECK(key_refs == expected_key_refs);

   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}


///////////////////////
// Wallet RPC
// Test adding an unnecessary signature to a transaction
///////////////////////
BOOST_FIXTURE_TEST_CASE(cli_sign_tx_with_unnecessary_signature, cli_fixture) {
   try {
      auto db = app1->chain_database();

      account_object rsquaredchp1_acct = con.wallet_api_ptr->get_account("rsquaredchp1");
      INVOKE(upgrade_rsquaredchp1_account);

      // Register Bob account
      const auto bob_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
              "bob", bob_bki.pub_key, bob_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );

      // Register Charlie account
      const graphene::wallet::brain_key_info charlie_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
              "charlie", charlie_bki.pub_key, charlie_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );
      const account_object &charlie_acc = con.wallet_api_ptr->get_account("charlie");

      // Import Bob's key
      BOOST_CHECK(con.wallet_api_ptr->import_key("bob", bob_bki.wif_priv_key));

      // Create transaction with a transfer operation from RSquaredCHP1 to Charlie
      transfer_operation top;
      top.from = rsquaredchp1_acct.id;
      top.to = charlie_acc.id;
      top.amount = asset(5000);
      top.fee = db->current_fee_schedule().calculate_fee(top);

      signed_transaction test_tx;
      test_tx.operations.push_back(top);

      // Sign the transaction with the implied rsquaredchp1's key and the explicitly yet unnecessary Bob's key
      auto signed_trx = con.wallet_api_ptr->sign_transaction2(test_tx, {bob_bki.pub_key}, false);

      // Check for two signatures on the transaction
      BOOST_CHECK_EQUAL(signed_trx.signatures.size(), 2);
      flat_set<public_key_type> signers = con.wallet_api_ptr->get_transaction_signers(signed_trx);

      // Check that the signed transaction contains both RSquaredCHP1's required signature and Bob's unnecessary signature
      BOOST_CHECK_EQUAL(rsquaredchp1_acct.active.get_keys().size(), 1);
      flat_set<public_key_type> expected_signers = {bob_bki.pub_key, rsquaredchp1_acct.active.get_keys().front()};
      flat_set<public_key_type> actual_signers = con.wallet_api_ptr->get_transaction_signers(signed_trx);
      BOOST_CHECK(signers == expected_signers);

   } catch (fc::exception &e) {
      edump((e.to_detail_string()));
      throw;
   }
}


///////////////////////
// Wallet RPC
// Test adding an unnecessary signature to a transaction builder
///////////////////////
BOOST_FIXTURE_TEST_CASE(cli_sign_tx_builder_with_unnecessary_signature, cli_fixture) {
   try {
      auto db = app1->chain_database();

      account_object rsquaredchp1_acct = con.wallet_api_ptr->get_account("rsquaredchp1");
      INVOKE(upgrade_rsquaredchp1_account);

      // Register Bob account
      const auto bob_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
              "bob", bob_bki.pub_key, bob_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );

      // Register Charlie account
      const graphene::wallet::brain_key_info charlie_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
              "charlie", charlie_bki.pub_key, charlie_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );
      const account_object &charlie_acc = con.wallet_api_ptr->get_account("charlie");

      // Import Bob's key
      BOOST_CHECK(con.wallet_api_ptr->import_key("bob", bob_bki.wif_priv_key));

      // Use transaction builder to build a transaction with a transfer operation from RSquaredCHP1 to Charlie
      graphene::wallet::transaction_handle_type tx_handle = con.wallet_api_ptr->begin_builder_transaction();

      transfer_operation top;
      top.from = rsquaredchp1_acct.id;
      top.to = charlie_acc.id;
      top.amount = asset(5000);

      con.wallet_api_ptr->add_operation_to_builder_transaction(tx_handle, top);
      con.wallet_api_ptr->set_fees_on_builder_transaction(tx_handle, GRAPHENE_SYMBOL);

      // Sign the transaction with the implied rsquaredchp1's key and the explicitly yet unnecessary Bob's key
      auto signed_trx = con.wallet_api_ptr->sign_builder_transaction(tx_handle, {bob_bki.pub_key}, false);

      // Check for two signatures on the transaction
      BOOST_CHECK_EQUAL(signed_trx.signatures.size(), 2);
      flat_set<public_key_type> signers = con.wallet_api_ptr->get_transaction_signers(signed_trx);

      // Check that the signed transaction contains both RSquaredCHP1's required signature and Bob's unnecessary signature
      BOOST_CHECK_EQUAL(rsquaredchp1_acct.active.get_keys().size(), 1);
      flat_set<public_key_type> expected_signers = {bob_bki.pub_key, rsquaredchp1_acct.active.get_keys().front()};
      flat_set<public_key_type> actual_signers = con.wallet_api_ptr->get_transaction_signers(signed_trx);
      BOOST_CHECK(signers == expected_signers);

   } catch (fc::exception &e) {
      edump((e.to_detail_string()));
      throw;
   }
}


BOOST_FIXTURE_TEST_CASE( cli_get_available_transaction_signers, cli_fixture )
{
   try
   {
      INVOKE(upgrade_rsquaredchp1_account);

      // register account
      const auto test_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
         "test", test_bki.pub_key, test_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );
      const auto &test_acc = con.wallet_api_ptr->get_account("test");

      // create and sign transaction
      signed_transaction trx;
      trx.operations = {transfer_operation()};

      // sign with test key
      const auto test_privkey = wif_to_key( test_bki.wif_priv_key );
      BOOST_REQUIRE( test_privkey );
      trx.sign( *test_privkey, con.wallet_data.chain_id );

      // sign with other keys
      const auto privkey_1 = fc::ecc::private_key::generate();
      trx.sign( privkey_1, con.wallet_data.chain_id );

      const auto privkey_2 = fc::ecc::private_key::generate();
      trx.sign( privkey_2, con.wallet_data.chain_id );

      // verify expected result
      flat_set<public_key_type> expected_signers = {test_bki.pub_key,
                                                    privkey_1.get_public_key(),
                                                    privkey_2.get_public_key()};

      auto signers = con.wallet_api_ptr->get_transaction_signers(trx);
      BOOST_CHECK(signers == expected_signers);

      // blockchain has no references to unknown accounts (privkey_1, privkey_2)
      // only test account available
      vector<flat_set<account_id_type> > expected_key_refs;
      expected_key_refs.push_back(flat_set<account_id_type>());
      expected_key_refs.push_back(flat_set<account_id_type>());
      expected_key_refs.push_back({test_acc.id});

      auto key_refs = con.wallet_api_ptr->get_key_references({expected_signers.begin(), expected_signers.end()});
      std::sort(key_refs.begin(), key_refs.end());

      BOOST_CHECK(key_refs == expected_key_refs);

   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_FIXTURE_TEST_CASE( cli_cant_get_signers_from_modified_transaction, cli_fixture )
{
   try
   {
      INVOKE(upgrade_rsquaredchp1_account);

      // register account
      const auto test_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
         "test", test_bki.pub_key, test_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );

      // create and sign transaction
      signed_transaction trx;
      trx.operations = {transfer_operation()};

      // sign with test key
      const auto test_privkey = wif_to_key( test_bki.wif_priv_key );
      BOOST_REQUIRE( test_privkey );
      trx.sign( *test_privkey, con.wallet_data.chain_id );

      // modify transaction (MITM-attack)
      trx.operations.clear();

      // verify if transaction has no valid signature of test account
      flat_set<public_key_type> expected_signers_of_valid_transaction = {test_bki.pub_key};
      auto signers = con.wallet_api_ptr->get_transaction_signers(trx);
      BOOST_CHECK(signers != expected_signers_of_valid_transaction);

   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

///////////////////
// Start a server and connect using the same calls as the CLI
// Set a voting proxy and be assured that it sticks
///////////////////
BOOST_FIXTURE_TEST_CASE( cli_set_voting_proxy, cli_fixture )
{
   try {
      INVOKE(create_new_account);

      // grab account for comparison
      account_object prior_voting_account = con.wallet_api_ptr->get_account("jmjatlanta");
      // set the voting proxy to rsquaredchp1
      BOOST_TEST_MESSAGE("About to set voting proxy.");
      signed_transaction voting_tx = con.wallet_api_ptr->set_voting_proxy("jmjatlanta", "rsquaredchp1", true);
      account_object after_voting_account = con.wallet_api_ptr->get_account("jmjatlanta");
      // see if it changed
      BOOST_CHECK(prior_voting_account.options.voting_account != after_voting_account.options.voting_account);
   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

/******
 * Check account history pagination (see bitshares-core/issue/1176)
 */
BOOST_FIXTURE_TEST_CASE( account_history_pagination, cli_fixture )
{
   try
   {
      INVOKE(create_new_account);

      // attempt to give jmjatlanta some rsquared
      BOOST_TEST_MESSAGE("Transferring rsquared from RSquaredCHP1 to jmjatlanta");
      for(int i = 1; i <= 199; i++)
      {
         signed_transaction transfer_tx = con.wallet_api_ptr->transfer("rsquaredchp1", "jmjatlanta", std::to_string(i),
                                                "1.3.0", "Here are some CORE token for your new account", true);
      }

      BOOST_CHECK(generate_block(app1));

      // now get account history and make sure everything is there (and no duplicates)
      std::vector<graphene::wallet::operation_detail> history = con.wallet_api_ptr->get_account_history("jmjatlanta", 300);
      BOOST_CHECK_EQUAL(201u, history.size() );

      std::set<object_id_type> operation_ids;

      for(auto& op : history)
      {
         if( operation_ids.find(op.op.id) != operation_ids.end() )
         {
            BOOST_FAIL("Duplicate found");
         }
         operation_ids.insert(op.op.id);
      }

      // Testing result formatter
      auto formatters = con.wallet_api_ptr->get_result_formatters();
      if( formatters.find("get_account_history") != formatters.end() )
      {
         BOOST_TEST_MESSAGE("Testing formatter of get_account_history");
         string output = formatters["get_account_history"](
               fc::variant(history, FC_PACK_MAX_DEPTH), fc::variants());
         BOOST_CHECK( output.find("Here are some") != string::npos );
      }
   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}


///////////////////////
// Create a multi-sig account and verify that only when all signatures are
// signed, the transaction could be broadcast
///////////////////////
BOOST_AUTO_TEST_CASE( cli_multisig_transaction )
{
   using namespace graphene::chain;
   using namespace graphene::app;
   std::shared_ptr<graphene::app::application> app1;
   try {
      fc::temp_directory app_dir( graphene::utilities::temp_directory_path() );

      int server_port_number = 0;
      app1 = start_application(app_dir, server_port_number);

      // connect to the server
      client_connection con(app1, app_dir, server_port_number);

      BOOST_TEST_MESSAGE("Setting wallet password");
      con.wallet_api_ptr->set_password("supersecret");
      con.wallet_api_ptr->unlock("supersecret");

      // import RSquaredCHP1 account
      BOOST_TEST_MESSAGE("Importing rsquaredchp1 key");
      std::vector<std::string> rsquaredchp1_keys{"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"};
      BOOST_CHECK_EQUAL(rsquaredchp1_keys[0], "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3");
      BOOST_CHECK(con.wallet_api_ptr->import_key("rsquaredchp1", rsquaredchp1_keys[0]));

      BOOST_TEST_MESSAGE("Importing rsquaredchp1's balance");
      std::vector<signed_transaction> import_txs = con.wallet_api_ptr->import_balance("rsquaredchp1", rsquaredchp1_keys, true);
      account_object rsquaredchp1_acct_before_upgrade = con.wallet_api_ptr->get_account("rsquaredchp1");

      // upgrade rsquaredchp1
      BOOST_TEST_MESSAGE("Upgrading RSquaredCHP1 to LTM");
      signed_transaction upgrade_tx = con.wallet_api_ptr->upgrade_account("rsquaredchp1", true);
      account_object rsquaredchp1_acct_after_upgrade = con.wallet_api_ptr->get_account("rsquaredchp1");

      // verify that the upgrade was successful
      BOOST_CHECK_PREDICATE( std::not_equal_to<uint32_t>(), (rsquaredchp1_acct_before_upgrade.membership_expiration_date.sec_since_epoch())(rsquaredchp1_acct_after_upgrade.membership_expiration_date.sec_since_epoch()) );
      BOOST_CHECK(rsquaredchp1_acct_after_upgrade.is_lifetime_member());

      // create a new multisig account
      graphene::wallet::brain_key_info bki1 = con.wallet_api_ptr->suggest_brain_key();
      graphene::wallet::brain_key_info bki2 = con.wallet_api_ptr->suggest_brain_key();
      graphene::wallet::brain_key_info bki3 = con.wallet_api_ptr->suggest_brain_key();
      graphene::wallet::brain_key_info bki4 = con.wallet_api_ptr->suggest_brain_key();
      BOOST_CHECK(!bki1.brain_priv_key.empty());
      BOOST_CHECK(!bki2.brain_priv_key.empty());
      BOOST_CHECK(!bki3.brain_priv_key.empty());
      BOOST_CHECK(!bki4.brain_priv_key.empty());

      signed_transaction create_multisig_acct_tx;
      account_create_operation account_create_op;

      account_create_op.referrer = rsquaredchp1_acct_after_upgrade.id;
      account_create_op.referrer_percent = rsquaredchp1_acct_after_upgrade.referrer_rewards_percentage;
      account_create_op.registrar = rsquaredchp1_acct_after_upgrade.id;
      account_create_op.name = "cifer.test";
      account_create_op.owner = authority(1, bki1.pub_key, 1);
      account_create_op.active = authority(2, bki2.pub_key, 1, bki3.pub_key, 1);
      account_create_op.options.memo_key = bki4.pub_key;
      account_create_op.fee = asset(1000000);  // should be enough for creating account

      create_multisig_acct_tx.operations.push_back(account_create_op);
      con.wallet_api_ptr->sign_transaction(create_multisig_acct_tx, true);

      // attempt to give cifer.test some rsquared
      BOOST_TEST_MESSAGE("Transferring rsquared from RSquaredCHP1 to cifer.test");
      signed_transaction transfer_tx1 = con.wallet_api_ptr->transfer("rsquaredchp1", "cifer.test", "10000", "1.3.0", "Here are some RQRX for your new account", true);

      // transfer bts from cifer.test to rsquaredchp1
      BOOST_TEST_MESSAGE("Transferring rsquared from cifer.test to rsquaredchp1");
      auto dyn_props = app1->chain_database()->get_dynamic_global_properties();
      account_object cifer_test = con.wallet_api_ptr->get_account("cifer.test");

      // construct a transfer transaction
      signed_transaction transfer_tx2;
      transfer_operation xfer_op;
      xfer_op.from = cifer_test.id;
      xfer_op.to = rsquaredchp1_acct_after_upgrade.id;
      xfer_op.amount = asset(100000000);
      xfer_op.fee = asset(3000000);  // should be enough for transfer
      transfer_tx2.operations.push_back(xfer_op);

      // case1: sign a transaction without TaPoS and expiration fields
      // expect: return a transaction with TaPoS and expiration filled
      transfer_tx2 =
         con.wallet_api_ptr->add_transaction_signature( transfer_tx2, false );
      BOOST_CHECK( ( transfer_tx2.ref_block_num != 0 &&
                     transfer_tx2.ref_block_prefix != 0 ) ||
                   ( transfer_tx2.expiration != fc::time_point_sec() ) );

      // case2: broadcast without signature
      // expect: exception with missing active authority
      BOOST_CHECK_THROW(con.wallet_api_ptr->broadcast_transaction(transfer_tx2), fc::exception);

      // case3:
      // import one of the private keys for this new account in the wallet file,
      // sign and broadcast with partial signatures
      //
      // expect: exception with missing active authority
      BOOST_CHECK(con.wallet_api_ptr->import_key("cifer.test", bki2.wif_priv_key));
      BOOST_CHECK_THROW(con.wallet_api_ptr->add_transaction_signature(transfer_tx2, true), fc::exception);

      // case4: sign again as signature exists
      // expect: num of signatures not increase
      transfer_tx2 = con.wallet_api_ptr->add_transaction_signature(transfer_tx2, false);
      BOOST_CHECK_EQUAL(transfer_tx2.signatures.size(), 1);

      // case5:
      // import another private key, sign and broadcast without full signatures
      //
      // expect: transaction broadcast successfully
      BOOST_CHECK(con.wallet_api_ptr->import_key("cifer.test", bki3.wif_priv_key));
      con.wallet_api_ptr->add_transaction_signature(transfer_tx2, true);
      auto balances = con.wallet_api_ptr->list_account_balances( "cifer.test" );
      for (auto b : balances) {
         if (b.asset_id == asset_id_type()) {
            BOOST_CHECK(b == asset(900000000 - 3000000));
         }
      }

   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

graphene::wallet::plain_keys decrypt_keys( const std::string& password, const vector<char>& cipher_keys )
{
   auto pw = fc::sha512::hash( password.c_str(), password.size() );
   vector<char> decrypted = fc::aes_decrypt( pw, cipher_keys );
   return fc::raw::unpack<graphene::wallet::plain_keys>( decrypted );
}

BOOST_AUTO_TEST_CASE( saving_keys_wallet_test ) {
   cli_fixture cli;

   cli.con.wallet_api_ptr->import_balance( "rsquaredchp1", cli.rsquaredchp1_keys, true );
   cli.con.wallet_api_ptr->upgrade_account( "rsquaredchp1", true );
   std::string brain_key( "FICTIVE WEARY MINIBUS LENS HAWKIE MAIDISH MINTY GLYPH GYTE KNOT COCKSHY LENTIGO PROPS BIFORM KHUTBAH BRAZIL" );
   cli.con.wallet_api_ptr->create_account_with_brain_key( brain_key, "account1", "rsquaredchp1", "rsquaredchp1", true );

   BOOST_CHECK_NO_THROW( cli.con.wallet_api_ptr->transfer( "rsquaredchp1", "account1", "9000", "1.3.0", "", true ) );

   std::string path( cli.app_dir.path().generic_string() + "/wallet.json" );
   graphene::wallet::wallet_data wallet = fc::json::from_file( path ).as<graphene::wallet::wallet_data>( 2 * GRAPHENE_MAX_NESTED_OBJECTS );
   BOOST_CHECK( wallet.extra_keys.size() == 1 ); // rsquaredchp1
   BOOST_CHECK( wallet.pending_account_registrations.size() == 1 ); // account1
   BOOST_CHECK( wallet.pending_account_registrations["account1"].size() == 2 ); // account1 active key + account1 memo key

   graphene::wallet::plain_keys pk = decrypt_keys( "supersecret", wallet.cipher_keys );
   BOOST_CHECK( pk.keys.size() == 1 ); // rsquaredchp1 key

   BOOST_CHECK( generate_block( cli.app1 ) );
   // Intentional delay
   fc::usleep( fc::seconds(1) );

   wallet = fc::json::from_file( path ).as<graphene::wallet::wallet_data>( 2 * GRAPHENE_MAX_NESTED_OBJECTS );
   BOOST_CHECK( wallet.extra_keys.size() == 2 ); // rsquaredchp1 + account1
   BOOST_CHECK( wallet.pending_account_registrations.empty() );
   BOOST_CHECK_NO_THROW( cli.con.wallet_api_ptr->transfer( "account1", "rsquaredchp1", "1000", "1.3.0", "", true ) );

   pk = decrypt_keys( "supersecret", wallet.cipher_keys );
   BOOST_CHECK( pk.keys.size() == 3 ); // rsquaredchp1 key + account1 active key + account1 memo key
}


///////////////////////
// Start a server and connect using the same calls as the CLI
// Create an HTLC
///////////////////////
BOOST_AUTO_TEST_CASE( cli_create_htlc )
{
   using namespace graphene::chain;
   using namespace graphene::app;
   std::shared_ptr<graphene::app::application> app1;
   try {
      fc::temp_directory app_dir( graphene::utilities::temp_directory_path() );

      int server_port_number = 0;
      app1 = start_application(app_dir, server_port_number);
      // set committee parameters
      app1->chain_database()->modify(app1->chain_database()->get_global_properties(), [](global_property_object& p) {
         graphene::chain::htlc_options params;
         params.max_preimage_size = 1024;
         params.max_timeout_secs = 60 * 60 * 24 * 28;
         p.parameters.extensions.value.updatable_htlc_options = params;
      });

      // connect to the server
      client_connection con(app1, app_dir, server_port_number);

      BOOST_TEST_MESSAGE("Setting wallet password");
      con.wallet_api_ptr->set_password("supersecret");
      con.wallet_api_ptr->unlock("supersecret");

      // import RSquaredCHP1 account
      BOOST_TEST_MESSAGE("Importing rsquaredchp1 key");
      std::vector<std::string> rsquaredchp1_keys{"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"};
      BOOST_CHECK_EQUAL(rsquaredchp1_keys[0], "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3");
      BOOST_CHECK(con.wallet_api_ptr->import_key("rsquaredchp1", rsquaredchp1_keys[0]));

      BOOST_TEST_MESSAGE("Importing rsquaredchp1's balance");
      std::vector<signed_transaction> import_txs = con.wallet_api_ptr->import_balance("rsquaredchp1", rsquaredchp1_keys, true);
      account_object rsquaredchp1_acct_before_upgrade = con.wallet_api_ptr->get_account("rsquaredchp1");

      // upgrade rsquaredchp1
      BOOST_TEST_MESSAGE("Upgrading RSquaredCHP1 to LTM");
      signed_transaction upgrade_tx = con.wallet_api_ptr->upgrade_account("rsquaredchp1", true);
      account_object rsquaredchp1_acct_after_upgrade = con.wallet_api_ptr->get_account("rsquaredchp1");

      // verify that the upgrade was successful
      BOOST_CHECK_PREDICATE( std::not_equal_to<uint32_t>(), (rsquaredchp1_acct_before_upgrade.membership_expiration_date.sec_since_epoch())
            (rsquaredchp1_acct_after_upgrade.membership_expiration_date.sec_since_epoch()) );
      BOOST_CHECK(rsquaredchp1_acct_after_upgrade.is_lifetime_member());

      // Create new asset called BOBCOIN
      try
      {
         graphene::chain::asset_options asset_ops;
         asset_ops.max_supply = 1000000;
         asset_ops.core_exchange_rate = price(asset(2),asset(1,asset_id_type(1)));
         fc::optional<graphene::chain::bitasset_options> bit_opts;
         con.wallet_api_ptr->create_asset("rsquaredchp1", "BOBCOIN", 5, asset_ops, bit_opts, true);
      }
      catch(exception& e)
      {
         BOOST_FAIL(e.what());
      }
      catch(...)
      {
         BOOST_FAIL("Unknown exception creating BOBCOIN");
      }

      // create a new account for Alice
      {
         graphene::wallet::brain_key_info bki = con.wallet_api_ptr->suggest_brain_key();
         BOOST_CHECK(!bki.brain_priv_key.empty());
         signed_transaction create_acct_tx = con.wallet_api_ptr->create_account_with_brain_key(bki.brain_priv_key,
               "alice", "rsquaredchp1", "rsquaredchp1", true);
         con.wallet_api_ptr->save_wallet_file(con.wallet_filename);
         // attempt to give alice some rsquared
         BOOST_TEST_MESSAGE("Transferring rsquared from RSquaredCHP1 to alice");
         signed_transaction transfer_tx = con.wallet_api_ptr->transfer("rsquaredchp1", "alice", "10000", "1.3.0",
               "Here are some CORE token for your new account", true);
      }

      // create a new account for Bob
      {
         graphene::wallet::brain_key_info bki = con.wallet_api_ptr->suggest_brain_key();
         BOOST_CHECK(!bki.brain_priv_key.empty());
         signed_transaction create_acct_tx = con.wallet_api_ptr->create_account_with_brain_key(bki.brain_priv_key,
               "bob", "rsquaredchp1", "rsquaredchp1", true);
         // this should cause resync which will import the keys of alice and bob
         generate_block(app1);
         // attempt to give bob some rsquared
         BOOST_TEST_MESSAGE("Transferring rsquared from RSquaredCHP1 to Bob");
         signed_transaction transfer_tx = con.wallet_api_ptr->transfer("rsquaredchp1", "bob", "10000", "1.3.0",
               "Here are some CORE token for your new account", true);
         con.wallet_api_ptr->issue_asset("bob", "5", "BOBCOIN", "Here are your BOBCOINs", true);
      }

      BOOST_TEST_MESSAGE("Alice has agreed to buy 3 BOBCOIN from Bob for 3 RQRX. Alice creates an HTLC");
      // create an HTLC
      std::string preimage_string = "My Secret";
      fc::sha256 preimage_md = fc::sha256::hash(preimage_string);
      std::stringstream ss;
      for(size_t i = 0; i < preimage_md.data_size(); i++)
      {
         char d = preimage_md.data()[i];
         unsigned char uc = static_cast<unsigned char>(d);
         ss << std::setfill('0') << std::setw(2) << std::hex << (int)uc;
      }
      std::string hash_str = ss.str();
      BOOST_TEST_MESSAGE("Secret is " + preimage_string + " and hash is " + hash_str);
      uint32_t timelock = fc::days(1).to_seconds();
      graphene::chain::signed_transaction result_tx
            = con.wallet_api_ptr->htlc_create("alice", "bob",
            "3", "1.3.0", "SHA256", hash_str, preimage_string.size(), timelock, "", true);

      // normally, a wallet would watch block production, and find the transaction. Here, we can cheat:
      std::string alice_htlc_id_as_string;
      {
         BOOST_TEST_MESSAGE("The system is generating a block");
         graphene::chain::signed_block result_block;
         BOOST_CHECK(generate_block(app1, result_block));

         // get the ID:
         auto tmp_hist = con.wallet_api_ptr->get_account_history("alice", 1);
         htlc_id_type htlc_id = tmp_hist[0].op.result.get<object_id_type>();
         alice_htlc_id_as_string = (std::string)(object_id_type)htlc_id;
         BOOST_TEST_MESSAGE("Alice shares the HTLC ID with Bob. The HTLC ID is: " + alice_htlc_id_as_string);
      }

      // Bob can now look over Alice's HTLC, to see if it is what was agreed to.
      BOOST_TEST_MESSAGE("Bob retrieves the HTLC Object by ID to examine it.");
      auto alice_htlc = con.wallet_api_ptr->get_htlc(alice_htlc_id_as_string);
      BOOST_TEST_MESSAGE("The HTLC Object is: " + fc::json::to_pretty_string(alice_htlc));

      // Bob likes what he sees, so he creates an HTLC, using the info he retrieved from Alice's HTLC
      con.wallet_api_ptr->htlc_create("bob", "alice",
            "3", "BOBCOIN", "SHA256", hash_str, preimage_string.size(), timelock, "", true);

      // normally, a wallet would watch block production, and find the transaction. Here, we can cheat:
      std::string bob_htlc_id_as_string;
      {
         BOOST_TEST_MESSAGE("The system is generating a block");
         graphene::chain::signed_block result_block;
         BOOST_CHECK(generate_block(app1, result_block));

         // get the ID:
         auto tmp_hist = con.wallet_api_ptr->get_account_history("bob", 1);
         htlc_id_type htlc_id = tmp_hist[0].op.result.get<object_id_type>();
         bob_htlc_id_as_string = (std::string)(object_id_type)htlc_id;
         BOOST_TEST_MESSAGE("Bob shares the HTLC ID with Alice. The HTLC ID is: " + bob_htlc_id_as_string);
      }

      // Alice can now look over Bob's HTLC, to see if it is what was agreed to:
      BOOST_TEST_MESSAGE("Alice retrieves the HTLC Object by ID to examine it.");
      auto bob_htlc = con.wallet_api_ptr->get_htlc(bob_htlc_id_as_string);
      BOOST_TEST_MESSAGE("The HTLC Object is: " + fc::json::to_pretty_string(bob_htlc));

      // Alice likes what she sees, so uses her preimage to get her BOBCOIN
      {
         BOOST_TEST_MESSAGE("Alice uses her preimage to retrieve the BOBCOIN");
         std::string secret = "My Secret";
         con.wallet_api_ptr->htlc_redeem(bob_htlc_id_as_string, "alice", secret, true);
         BOOST_TEST_MESSAGE("The system is generating a block");
         BOOST_CHECK(generate_block(app1));
      }

      // TODO: Bob can look at Alice's history to see her preimage
      // Bob can use the preimage to retrieve his RQRX
      {
         BOOST_TEST_MESSAGE("Bob uses Alice's preimage to retrieve the BOBCOIN");
         std::string secret = "My Secret";
         con.wallet_api_ptr->htlc_redeem(alice_htlc_id_as_string, "bob", secret, true);
         BOOST_TEST_MESSAGE("The system is generating a block");
         BOOST_CHECK(generate_block(app1));
      }

      // test operation_printer
      auto hist = con.wallet_api_ptr->get_account_history("alice", 10);
      for(size_t i = 0; i < hist.size(); ++i)
      {
         auto obj = hist[i];
         std::stringstream ss;
         ss << "Description: " << obj.description << "\n";
         auto str = ss.str();
         BOOST_TEST_MESSAGE( str );
         if (i == 3 || i == 4)
         {
            BOOST_CHECK( str.find("SHA256 8a45f62f47") != std::string::npos );
         }
      }
   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

static string encapsulate( const graphene::wallet::signed_message& msg )
{
   fc::stringstream encapsulated;
   encapsulated << "-----BEGIN RSQUARED SIGNED MESSAGE-----\n"
                << msg.message << '\n'
                << "-----BEGIN META-----\n"
                << "account=" << msg.meta.account << '\n'
                << "memokey=" << std::string( msg.meta.memo_key ) << '\n'
                << "block=" << msg.meta.block << '\n'
                << "timestamp=" << msg.meta.time << '\n'
                << "-----BEGIN SIGNATURE-----\n"
                << fc::to_hex( (const char*)msg.signature->data(), msg.signature->size() ) << '\n'
                << "-----END RSQUARED SIGNED MESSAGE-----";
   return encapsulated.str();
}

/******
 * Check signing/verifying a message with a memo key
 */
BOOST_FIXTURE_TEST_CASE( cli_sign_message, cli_fixture )
{ try {
   const auto rsquaredchp1_priv = *wif_to_key( rsquaredchp1_keys[0] );
   const public_key_type rsquaredchp1_pub( rsquaredchp1_priv.get_public_key() );

   // account does not exist
   BOOST_REQUIRE_THROW( con.wallet_api_ptr->sign_message( "dan", "123" ), fc::assert_exception );

   // success
   graphene::wallet::signed_message msg = con.wallet_api_ptr->sign_message( "rsquaredchp1", "123" );
   BOOST_CHECK_EQUAL( "123", msg.message );
   BOOST_CHECK_EQUAL( "rsquaredchp1", msg.meta.account );
   BOOST_CHECK_EQUAL( std::string( rsquaredchp1_pub ), std::string( msg.meta.memo_key ) );
   BOOST_CHECK( msg.signature.valid() );

   // change message, verify failure
   msg.message = "124";
   BOOST_CHECK( !con.wallet_api_ptr->verify_message( msg.message, msg.meta.account, msg.meta.block, msg.meta.time,
                                                     *msg.signature ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_signed_message( msg ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_encapsulated_message( encapsulate( msg ) ) );
   msg.message = "123";

   // change account, verify failure
   // nonexistent account:
   msg.meta.account = "dan";
   BOOST_REQUIRE_THROW( con.wallet_api_ptr->verify_message( msg.message, msg.meta.account, msg.meta.block,
                                                            msg.meta.time, *msg.signature ), fc::assert_exception );
   BOOST_REQUIRE_THROW( con.wallet_api_ptr->verify_signed_message( msg ), fc::assert_exception );
   BOOST_REQUIRE_THROW( con.wallet_api_ptr->verify_encapsulated_message( encapsulate( msg ) ), fc::assert_exception);
   // existing, but wrong account:
   msg.meta.account = "committee-account";
   BOOST_CHECK( !con.wallet_api_ptr->verify_message( msg.message, msg.meta.account, msg.meta.block,
                                                     msg.meta.time, *msg.signature ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_signed_message( msg ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_encapsulated_message( encapsulate( msg ) ) );
   msg.meta.account = "rsquaredchp1";

   // change key, verify failure
   ++msg.meta.memo_key.key_data.data()[1];
   //BOOST_CHECK( !con.wallet_api_ptr->verify_message( msg.message, msg.meta.account, msg.meta.block, msg.meta.time,
   //                                                  *msg.signature ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_signed_message( msg ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_encapsulated_message( encapsulate( msg ) ) );
   --msg.meta.memo_key.key_data.data()[1];

   // change block, verify failure
   ++msg.meta.block;
   BOOST_CHECK( !con.wallet_api_ptr->verify_message( msg.message, msg.meta.account, msg.meta.block, msg.meta.time,
                                                     *msg.signature ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_signed_message( msg ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_encapsulated_message( encapsulate( msg ) ) );
   --msg.meta.block;

   // change time, verify failure
   ++msg.meta.time[0];
   BOOST_CHECK( !con.wallet_api_ptr->verify_message( msg.message, msg.meta.account, msg.meta.block, msg.meta.time,
                                                     *msg.signature ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_signed_message( msg ) );
   BOOST_CHECK( !con.wallet_api_ptr->verify_encapsulated_message( encapsulate( msg ) ) );
   --msg.meta.time[0];

   // change signature, verify failure
   ++msg.signature->data()[1];
   try {
      BOOST_CHECK( !con.wallet_api_ptr->verify_message( msg.message, msg.meta.account, msg.meta.block, msg.meta.time,
                                                        *msg.signature ) );
   } catch( const fc::assert_exception& ) {} // failure to reconstruct key from signature is ok as well
   try {
      BOOST_CHECK( !con.wallet_api_ptr->verify_signed_message( msg ) );
   } catch( const fc::assert_exception& ) {} // failure to reconstruct key from signature is ok as well
   try {
      BOOST_CHECK( !con.wallet_api_ptr->verify_encapsulated_message( encapsulate( msg ) ) );
   } catch( const fc::assert_exception& ) {} // failure to reconstruct key from signature is ok as well
   --msg.signature->data()[1];

   // verify success
   BOOST_CHECK( con.wallet_api_ptr->verify_message( msg.message, msg.meta.account, msg.meta.block, msg.meta.time,
                                                    *msg.signature ) );
   BOOST_CHECK( con.wallet_api_ptr->verify_signed_message( msg ) );
   BOOST_CHECK( con.wallet_api_ptr->verify_encapsulated_message( encapsulate( msg ) ) );

} FC_LOG_AND_RETHROW() }

///////////////////
// Test the general storage by custom operations plugin
///////////////////
BOOST_FIXTURE_TEST_CASE( general_storage, cli_fixture )
{
   try {
      // create the taker account
      INVOKE(create_new_account);

      auto db = app1->chain_database();

      BOOST_TEST_MESSAGE("Storing in a map.");

      flat_map<string, optional<string>> pairs;
      pairs["key1"] = fc::json::to_string("value1");
      pairs["key2"] = fc::json::to_string("value2");

      con.wallet_api_ptr->account_store_map("rsquaredchp1", "any", false, pairs, true);

      BOOST_TEST_MESSAGE("The system is generating a block.");
      BOOST_CHECK(generate_block(app1));

      BOOST_TEST_MESSAGE("Get current map for rsquaredchp1.");
      auto rsquaredchp1_map = con.wallet_api_ptr->get_account_storage("rsquaredchp1", "any");

      BOOST_CHECK_EQUAL(rsquaredchp1_map[0].id.instance(), 0);
      BOOST_CHECK_EQUAL(rsquaredchp1_map[0].account.instance.value, 17);
      BOOST_CHECK_EQUAL(rsquaredchp1_map[0].catalog, "any");
      BOOST_CHECK_EQUAL(rsquaredchp1_map[0].key, "key1");
      BOOST_CHECK_EQUAL(rsquaredchp1_map[0].value->as_string(), "value1");
      BOOST_CHECK_EQUAL(rsquaredchp1_map[1].id.instance(), 1);
      BOOST_CHECK_EQUAL(rsquaredchp1_map[1].account.instance.value, 17);
      BOOST_CHECK_EQUAL(rsquaredchp1_map[1].catalog, "any");
      BOOST_CHECK_EQUAL(rsquaredchp1_map[1].key, "key2");
      BOOST_CHECK_EQUAL(rsquaredchp1_map[1].value->as_string(), "value2");

      BOOST_TEST_MESSAGE("Storing in a list.");

      flat_map<string, optional<string>> favs;
      favs["chocolate"];
      favs["milk"];
      favs["banana"];

      con.wallet_api_ptr->account_store_map("rsquaredchp1", "favourites", false, favs, true);

      BOOST_TEST_MESSAGE("The system is generating a block.");
      BOOST_CHECK(generate_block(app1));

      BOOST_TEST_MESSAGE("Get current list for rsquaredchp1.");
      auto rsquaredchp1_list = con.wallet_api_ptr->get_account_storage("rsquaredchp1", "favourites");

      BOOST_CHECK_EQUAL(rsquaredchp1_list[0].id.instance(), 2);
      BOOST_CHECK_EQUAL(rsquaredchp1_list[0].account.instance.value, 17);
      BOOST_CHECK_EQUAL(rsquaredchp1_list[0].catalog, "favourites");
      BOOST_CHECK_EQUAL(rsquaredchp1_list[0].key, "banana");
      BOOST_CHECK_EQUAL(rsquaredchp1_list[1].id.instance(), 3);
      BOOST_CHECK_EQUAL(rsquaredchp1_list[1].account.instance.value, 17);
      BOOST_CHECK_EQUAL(rsquaredchp1_list[1].catalog, "favourites");
      BOOST_CHECK_EQUAL(rsquaredchp1_list[1].key, "chocolate");
      BOOST_CHECK_EQUAL(rsquaredchp1_list[2].id.instance(), 4);
      BOOST_CHECK_EQUAL(rsquaredchp1_list[2].account.instance.value, 17);
      BOOST_CHECK_EQUAL(rsquaredchp1_list[2].catalog, "favourites");
      BOOST_CHECK_EQUAL(rsquaredchp1_list[2].key, "milk");

   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}

//////
// Template copied
//////
template<typename Object>
unsigned_int member_index(string name) {
   unsigned_int index;
   fc::typelist::runtime::for_each(typename fc::reflector<Object>::native_members(), [&name, &index](auto t) mutable {
      if (name == decltype(t)::type::get_name())
      index = decltype(t)::type::index;
   });
   return index;
}

///////////////////////
// Wallet RPC
// Test sign_builder_transaction with an account (bob) that has received a custom authorization
// to transfer funds from another account (alice)
///////////////////////
BOOST_FIXTURE_TEST_CASE(cli_use_authorized_transfer, cli_fixture) {
   try {
      //////
      // Initialize the blockchain
      //////
      auto db = app1->chain_database();

      account_object rsquaredchp1_acct = con.wallet_api_ptr->get_account("rsquaredchp1");
      INVOKE(upgrade_rsquaredchp1_account);

      // Register Alice account
      const auto alice_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
              "alice", alice_bki.pub_key, alice_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );
      const account_object &alice_acc = con.wallet_api_ptr->get_account("alice");

      // Register Bob account
      const auto bob_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
              "bob", bob_bki.pub_key, bob_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );
      const account_object &bob_acc = con.wallet_api_ptr->get_account("bob");

      // Register Charlie account
      const graphene::wallet::brain_key_info charlie_bki = con.wallet_api_ptr->suggest_brain_key();
      con.wallet_api_ptr->register_account(
              "charlie", charlie_bki.pub_key, charlie_bki.pub_key, "rsquaredchp1", "rsquaredchp1", 0, true
      );
      const account_object &charlie_acc = con.wallet_api_ptr->get_account("charlie");

      // Fund Alice's account
      con.wallet_api_ptr->transfer("rsquaredchp1", "alice", "450000", "1.3.0", "", true);

      // Initialize common variables
      signed_transaction signed_trx;


      //////
      // Initialize Alice's CLI wallet
      //////
      client_connection con_alice(app1, app_dir, server_port_number, "wallet_alice.json");
      con_alice.wallet_api_ptr->set_password("supersecret");
      con_alice.wallet_api_ptr->unlock("supersecret");

      // Import Alice's key
      BOOST_CHECK(con_alice.wallet_api_ptr->import_key("alice", alice_bki.wif_priv_key));


      //////
      // Initialize the blockchain for BSIP 40
      //////
      generate_blocks(app1, HARDFORK_BSIP_40_TIME);
      // Set committee parameters
      app1->chain_database()->modify(app1->chain_database()->get_global_properties(), [](global_property_object& p) {
         p.parameters.extensions.value.custom_authority_options = custom_authority_options_type();
      });


      //////
      // Alice authorizes Bob to transfer funds from her account to Charlie's account
      //////
      graphene::wallet::transaction_handle_type tx_alice_handle = con_alice.wallet_api_ptr->begin_builder_transaction();

      custom_authority_create_operation caop;
      caop.account = alice_acc.get_id();
      caop.auth.add_authority(bob_acc.get_id(), 1);
      caop.auth.weight_threshold = 1;
      caop.enabled = true;
      caop.valid_to = db->head_block_time() + 1000;
      caop.operation_type = operation::tag<transfer_operation>::value;

      // Restriction should have "to" equal Charlie
      vector<restriction> restrictions;
      auto to_index = member_index<transfer_operation>("to");
      restrictions.emplace_back(to_index, restriction::func_eq, charlie_acc.get_id());

      con_alice.wallet_api_ptr->add_operation_to_builder_transaction(tx_alice_handle, caop);
      asset ca_fee = con_alice.wallet_api_ptr->set_fees_on_builder_transaction(tx_alice_handle, GRAPHENE_SYMBOL);

      // Sign the transaction with the inferred Alice key
      signed_trx = con_alice.wallet_api_ptr->sign_builder_transaction(tx_alice_handle, {}, true);

      // Check for one signatures on the transaction
      BOOST_CHECK_EQUAL(signed_trx.signatures.size(), 1);

      // Check that the signed transaction contains Alice's signature
      flat_set<public_key_type> expected_signers = {alice_bki.pub_key};
      flat_set<public_key_type> actual_signers = con_alice.wallet_api_ptr->get_transaction_signers(signed_trx);
      BOOST_CHECK(actual_signers == expected_signers);


      //////
      // Initialize Bob's CLI wallet
      //////
      client_connection con_bob(app1, app_dir, server_port_number, "wallet_bob.json");
      con_bob.wallet_api_ptr->set_password("supersecret");
      con_bob.wallet_api_ptr->unlock("supersecret");

      // Import Bob's key
      BOOST_CHECK(con_bob.wallet_api_ptr->import_key("bob", bob_bki.wif_priv_key));


      //////
      // Bob attempt to transfer funds from Alice to Charlie while using Bob's wallet
      // This should succeed because Bob is authorized to transfer by Alice
      //////
      graphene::wallet::transaction_handle_type tx_bob_handle = con_bob.wallet_api_ptr->begin_builder_transaction();

      const asset transfer_amount = asset(123 * GRAPHENE_BLOCKCHAIN_PRECISION);
      transfer_operation top;
      top.from = alice_acc.id;
      top.to = charlie_acc.id;
      top.amount = transfer_amount;

      con_bob.wallet_api_ptr->add_operation_to_builder_transaction(tx_bob_handle, top);
      asset transfer_fee = con_bob.wallet_api_ptr->set_fees_on_builder_transaction(tx_bob_handle, GRAPHENE_SYMBOL);

      // Sign the transaction with the explicit Bob key
      signed_trx = con_bob.wallet_api_ptr->sign_builder_transaction(tx_bob_handle, {bob_bki.pub_key}, true);

      // Check for one signatures on the transaction
      BOOST_CHECK_EQUAL(signed_trx.signatures.size(), 1);

      // Check that the signed transaction contains Bob's signature
      BOOST_CHECK_EQUAL(rsquaredchp1_acct.active.get_keys().size(), 1);
      expected_signers = {bob_bki.pub_key};
      actual_signers = con_bob.wallet_api_ptr->get_transaction_signers(signed_trx);
      BOOST_CHECK(actual_signers == expected_signers);


      //////
      // Check account balances
      //////
      // Check Charlie's balances
      vector<asset> charlie_balances = con.wallet_api_ptr->list_account_balances("charlie");
      BOOST_CHECK_EQUAL(charlie_balances.size(), 1);
      asset charlie_core_balance = charlie_balances.front();
      asset expected_charlie_core_balance = transfer_amount;
      BOOST_CHECK(charlie_core_balance == expected_charlie_core_balance);

      // Check Bob's balances
      vector<asset> bob_balances = con.wallet_api_ptr->list_account_balances("bob");
      BOOST_CHECK_EQUAL(bob_balances.size(), 0);

      // Check Alice's balance
      vector<asset> alice_balances = con.wallet_api_ptr->list_account_balances("alice");
      BOOST_CHECK_EQUAL(alice_balances.size(), 1);
      asset alice_core_balance = alice_balances.front();
      asset expected_alice_balance =  asset(450000 * GRAPHENE_BLOCKCHAIN_PRECISION)
                                      - expected_charlie_core_balance
                                      - ca_fee - transfer_fee;
      BOOST_CHECK(alice_core_balance.asset_id == expected_alice_balance.asset_id);
      BOOST_CHECK_EQUAL(alice_core_balance.amount.value, expected_alice_balance.amount.value);

   } catch (fc::exception &e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( cli_create_htlc_bsip64 )
{
   using namespace graphene::chain;
   using namespace graphene::app;
   std::shared_ptr<graphene::app::application> app1;
   try {
      fc::temp_directory app_dir( graphene::utilities::temp_directory_path() );

      int server_port_number = 0;
      app1 = start_application(app_dir, server_port_number);
      // set committee parameters
      app1->chain_database()->modify(app1->chain_database()->get_global_properties(), [](global_property_object& p)
      {
         graphene::chain::htlc_options params;
         params.max_preimage_size = 1024;
         params.max_timeout_secs = 60 * 60 * 24 * 28;
         p.parameters.extensions.value.updatable_htlc_options = params;
      });

      // connect to the server
      client_connection con(app1, app_dir, server_port_number);

      BOOST_TEST_MESSAGE("Setting wallet password");
      con.wallet_api_ptr->set_password("supersecret");
      con.wallet_api_ptr->unlock("supersecret");

      // import RSquaredCHP1 account
      BOOST_TEST_MESSAGE("Importing rsquaredchp1 key");
      std::vector<std::string> rsquaredchp1_keys{"5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"};
      BOOST_CHECK_EQUAL(rsquaredchp1_keys[0], "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3");
      BOOST_CHECK(con.wallet_api_ptr->import_key("rsquaredchp1", rsquaredchp1_keys[0]));

      BOOST_TEST_MESSAGE("Importing rsquaredchp1's balance");
      std::vector<signed_transaction> import_txs = con.wallet_api_ptr->import_balance("rsquaredchp1", rsquaredchp1_keys, true);
      account_object rsquaredchp1_acct_before_upgrade = con.wallet_api_ptr->get_account("rsquaredchp1");

      // upgrade rsquaredchp1
      BOOST_TEST_MESSAGE("Upgrading RSquaredCHP1 to LTM");
      signed_transaction upgrade_tx = con.wallet_api_ptr->upgrade_account("rsquaredchp1", true);
      account_object rsquaredchp1_acct_after_upgrade = con.wallet_api_ptr->get_account("rsquaredchp1");

      // verify that the upgrade was successful
      BOOST_CHECK_PREDICATE( std::not_equal_to<uint32_t>(),
            (rsquaredchp1_acct_before_upgrade.membership_expiration_date.sec_since_epoch())
            (rsquaredchp1_acct_after_upgrade.membership_expiration_date.sec_since_epoch()) );
      BOOST_CHECK(rsquaredchp1_acct_after_upgrade.is_lifetime_member());

      // Create new asset called BOBCOIN
      try
      {
         graphene::chain::asset_options asset_ops;
         asset_ops.max_supply = 1000000;
         asset_ops.core_exchange_rate = price(asset(2),asset(1,asset_id_type(1)));
         fc::optional<graphene::chain::bitasset_options> bit_opts;
         con.wallet_api_ptr->create_asset("rsquaredchp1", "BOBCOIN", 5, asset_ops, bit_opts, true);
      }
      catch(exception& e)
      {
         BOOST_FAIL(e.what());
      }
      catch(...)
      {
         BOOST_FAIL("Unknown exception creating BOBCOIN");
      }

      // create a new account for Alice
      {
         graphene::wallet::brain_key_info bki = con.wallet_api_ptr->suggest_brain_key();
         BOOST_CHECK(!bki.brain_priv_key.empty());
         signed_transaction create_acct_tx = con.wallet_api_ptr->create_account_with_brain_key(bki.brain_priv_key,
               "alice", "rsquaredchp1", "rsquaredchp1", true);
         con.wallet_api_ptr->save_wallet_file(con.wallet_filename);
         // attempt to give alice some rsquared
         BOOST_TEST_MESSAGE("Transferring rsquared from RSquaredCHP1 to alice");
         signed_transaction transfer_tx = con.wallet_api_ptr->transfer("rsquaredchp1", "alice", "10000", "1.3.0",
               "Here are some CORE token for your new account", true);
      }

      // create a new account for Bob
      {
         graphene::wallet::brain_key_info bki = con.wallet_api_ptr->suggest_brain_key();
         BOOST_CHECK(!bki.brain_priv_key.empty());
         signed_transaction create_acct_tx = con.wallet_api_ptr->create_account_with_brain_key(bki.brain_priv_key,
               "bob", "rsquaredchp1", "rsquaredchp1", true);
         // this should cause resync which will import the keys of alice and bob
         generate_block(app1);
         // attempt to give bob some rsquared
         BOOST_TEST_MESSAGE("Transferring rsquared from RSquaredCHP1 to Bob");
         signed_transaction transfer_tx = con.wallet_api_ptr->transfer("rsquaredchp1", "bob", "10000", "1.3.0",
               "Here are some CORE token for your new account", true);
         con.wallet_api_ptr->issue_asset("bob", "5", "BOBCOIN", "Here are your BOBCOINs", true);
      }

      BOOST_TEST_MESSAGE("Alice has agreed to buy 3 BOBCOIN from Bob for 3 RQRX. Alice creates an HTLC");
      // create an HTLC
      std::string preimage_string = "My Super Long Secret that is larger than 50 charaters. How do I look?\n";
      fc::hash160 preimage_md = fc::hash160::hash(preimage_string);
      std::stringstream ss;
      for(size_t i = 0; i < preimage_md.data_size(); i++)
      {
         char d = preimage_md.data()[i];
         unsigned char uc = static_cast<unsigned char>(d);
         ss << std::setfill('0') << std::setw(2) << std::hex << (int)uc;
      }
      std::string hash_str = ss.str();
      BOOST_TEST_MESSAGE("Secret is " + preimage_string + " and hash is " + hash_str);
      uint32_t timelock = fc::days(1).to_seconds();
      graphene::chain::signed_transaction result_tx
            = con.wallet_api_ptr->htlc_create("alice", "bob",
            "3", "1.3.0", "HASH160", hash_str, preimage_string.size(), timelock, "Alice to Bob", true);

      // normally, a wallet would watch block production, and find the transaction. Here, we can cheat:
      std::string alice_htlc_id_as_string;
      {
         BOOST_TEST_MESSAGE("The system is generating a block");
         graphene::chain::signed_block result_block;
         BOOST_CHECK(generate_block(app1, result_block));

         // get the ID:
         auto tmp_hist = con.wallet_api_ptr->get_account_history("alice", 1);
         htlc_id_type htlc_id = tmp_hist[0].op.result.get<object_id_type>();
         alice_htlc_id_as_string = (std::string)(object_id_type)htlc_id;
         BOOST_TEST_MESSAGE("Alice shares the HTLC ID with Bob. The HTLC ID is: " + alice_htlc_id_as_string);
      }

      // Bob can now look over Alice's HTLC, to see if it is what was agreed to.
      BOOST_TEST_MESSAGE("Bob retrieves the HTLC Object by ID to examine it.");
      auto alice_htlc = con.wallet_api_ptr->get_htlc(alice_htlc_id_as_string);
      BOOST_TEST_MESSAGE("The HTLC Object is: " + fc::json::to_pretty_string(alice_htlc));

      // Bob likes what he sees, so he creates an HTLC, using the info he retrieved from Alice's HTLC
      con.wallet_api_ptr->htlc_create("bob", "alice",
            "3", "BOBCOIN", "HASH160", hash_str, preimage_string.size(), fc::hours(12).to_seconds(),
            "Bob to Alice", true);

      // normally, a wallet would watch block production, and find the transaction. Here, we can cheat:
      std::string bob_htlc_id_as_string;
      {
         BOOST_TEST_MESSAGE("The system is generating a block");
         graphene::chain::signed_block result_block;
         BOOST_CHECK(generate_block(app1, result_block));

         // get the ID:
         auto tmp_hist = con.wallet_api_ptr->get_account_history("bob", 1);
         htlc_id_type htlc_id = tmp_hist[0].op.result.get<object_id_type>();
         bob_htlc_id_as_string = (std::string)(object_id_type)htlc_id;
         BOOST_TEST_MESSAGE("Bob shares the HTLC ID with Alice. The HTLC ID is: " + bob_htlc_id_as_string);
      }

      // Alice can now look over Bob's HTLC, to see if it is what was agreed to:
      BOOST_TEST_MESSAGE("Alice retrieves the HTLC Object by ID to examine it.");
      auto bob_htlc = con.wallet_api_ptr->get_htlc(bob_htlc_id_as_string);
      BOOST_TEST_MESSAGE("The HTLC Object is: " + fc::json::to_pretty_string(bob_htlc));

      // Alice likes what she sees, so uses her preimage to get her BOBCOIN
      {
         BOOST_TEST_MESSAGE("Alice uses her preimage to retrieve the BOBCOIN");
         con.wallet_api_ptr->htlc_redeem(bob_htlc_id_as_string, "alice", preimage_string, true);
         BOOST_TEST_MESSAGE("The system is generating a block");
         BOOST_CHECK(generate_block(app1));
      }

      // Bob can look at Alice's history to see her preimage
      {
         BOOST_TEST_MESSAGE("Bob can look at the history of Alice to see the preimage");
         std::vector<graphene::wallet::operation_detail> hist = con.wallet_api_ptr->get_account_history("alice", 1);
         BOOST_CHECK( hist[0].description.find("with preimage \"4d792") != hist[0].description.npos);
      }

      // Bob can also look at his own history to see Alice's preimage
      {
         BOOST_TEST_MESSAGE("Bob can look at his own history to see the preimage");
         std::vector<graphene::wallet::operation_detail> hist = con.wallet_api_ptr->get_account_history("bob", 1);
         BOOST_CHECK( hist[0].description.find("with preimage \"4d792") != hist[0].description.npos);
      }

      // Bob can use the preimage to retrieve his RQRX
      {
         BOOST_TEST_MESSAGE("Bob uses Alice's preimage to retrieve the BOBCOIN");
         con.wallet_api_ptr->htlc_redeem(alice_htlc_id_as_string, "bob", preimage_string, true);
         BOOST_TEST_MESSAGE("The system is generating a block");
         BOOST_CHECK(generate_block(app1));
      }

      // test operation_printer
      auto hist = con.wallet_api_ptr->get_account_history("alice", 10);
      for(size_t i = 0; i < hist.size(); ++i)
      {
         auto obj = hist[i];
         std::stringstream ss;
         ss << "Description: " << obj.description << "\n";
         auto str = ss.str();
         BOOST_TEST_MESSAGE( str );
         if (i == 3 || i == 4)
         {
            BOOST_CHECK( str.find("HASH160 620e4d5ba") != std::string::npos );
         }
      }
      con.wallet_api_ptr->lock();
      hist = con.wallet_api_ptr->get_account_history("alice", 10);
      for(size_t i = 0; i < hist.size(); ++i)
      {
         auto obj = hist[i];
         std::stringstream ss;
         ss << "Description: " << obj.description << "\n";
         auto str = ss.str();
         BOOST_TEST_MESSAGE( str );
         if (i == 3 || i == 4)
         {
            BOOST_CHECK( str.find("HASH160 620e4d5ba") != std::string::npos );
         }
      }
      con.wallet_api_ptr->unlock("supersecret");

   } catch( fc::exception& e ) {
      edump((e.to_detail_string()));
      throw;
   }
}
