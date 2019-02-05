/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#pragma once

#include <eosiolib/asset.hpp>
#include <eosiolib/eosio.hpp>
#include <eosiolib/time.hpp>

#include <string>

// time in seconds
const uint32_t ONE_MINUTE = 60;
const uint32_t ONE_HOUR = ONE_MINUTE * 60;
const uint32_t ONE_DAY = ONE_HOUR * 24;
const uint32_t ONE_YEAR = ONE_DAY * 365;

namespace eosio {

   using std::string;

   class [[eosio::contract("iscoinalpha1")]] token : public contract {
      public:
         using contract::contract;

         [[eosio::action]]
         void create( asset  maximum_supply);

         [[eosio::action]]
         void transfer( name    from,
                        name    to,
                        asset   quantity,
                        string  memo );

         [[eosio::action]]
         void open( name owner, const symbol& symbol, name ram_payer );

         [[eosio::action]]
         void close( name owner, const symbol& symbol );

         [[eosio::action]]
         void addstake( name     staker,
                        asset    quantity,
                        size_t   duration_index );

         [[eosio::action]]
         void update( const symbol& symbol );

         static asset get_supply( name token_contract_account, symbol_code sym_code )
         {
            stats statstable( token_contract_account, sym_code.raw() );
            const auto& st = statstable.get( sym_code.raw() );
            return st.supply;
         }

         static asset get_balance( name token_contract_account, name owner, symbol_code sym_code )
         {
            accounts accountstable( token_contract_account, owner.value );
            const auto& ac = accountstable.get( sym_code.raw() );
            return ac.balance;
         }

      private:
         struct [[eosio::table]] account {
            asset    balance;

            uint64_t primary_key()const { return balance.symbol.code().raw(); }
         };

         struct [[eosio::table]] currency_stats {
            asset                   supply;
            asset                   max_supply;
            eosio::time_point_sec   created;
            eosio::time_point_sec   updated;
            uint16_t                boosts; // number of boosts so far

            uint64_t primary_key()const { return supply.symbol.code().raw(); }
         };

         struct [[eosio::table]] stake {
            uint64_t                id; // use available_primary_key() to generate
            asset                   quantity;
            eosio::time_point_sec   start;
            size_t                  duration_index;

            uint64_t primary_key()const { return id; }
         };

         struct [[eosio::table]] stake_stat {
            name           staker;
            asset          total_stake;
            int64_t        stake_weight;

            uint64_t primary_key()const { return staker.value; }
         };

         typedef eosio::multi_index< "accounts"_n, account > accounts;
         typedef eosio::multi_index< "stat"_n, currency_stats > stats;
         typedef eosio::multi_index< "stakes"_n, stake> stakes;
         typedef eosio::multi_index< "stakestats"_n, stake_stat> stake_stats;

         void issue( asset quantity );
         void sub_balance( name owner, asset value );
         void add_balance( name owner, asset value, name ram_payer );

         void update_stakes( const symbol& symbol );
         void update_boost( const symbol& symbol );
         const uint32_t update_interval = ONE_MINUTE;

         // distribution

         const float ISSUE_PROPORTION = 0.75f; // remainder used for boost
         inline float boost_proportion()
         {
            return 1.0f - ISSUE_PROPORTION;
         }

         // staking

         static const size_t stake_count = 6;
         // short durations for testing
         // TODO: change to months, not minutes
         const uint32_t stake_durations[stake_count] = {
            1 * ONE_MINUTE, // 1 month
            3 * ONE_MINUTE, // 2 months
            6 * ONE_MINUTE, // 6 months
            12  * ONE_MINUTE, // 1 year
            12 * 2  * ONE_MINUTE, // 2 years
            12 * 5 * ONE_MINUTE, // 5 years
         };
         const int64_t stake_weights[stake_count] = {
            50,
            60,
            75,
            100,
            100,
            100,
         };

         asset get_stake( name owner, const symbol& symbol )const;
         int64_t get_stake_weight( name owner, const symbol& symbol )const;
         asset get_unstaked_balance( name owner, const symbol& symbol )const;

         // transaction fee

         const float transaction_fee = 0.01; // 1%
         const float transaction_fee_to_stakers = 0.7f; // 70% of the transaction fee
         // const float transaction_fee_to_likes = 0.15f; // 15%
         // this account gets the rest

         int64_t distribute( asset quantity );

         // boost
         // TODO: change to weekly
         const uint32_t boost_interval = ONE_MINUTE * 2;
         const uint16_t boost_count = 312; // total number of boosts
         const float    boost_lambda = -0.015f;
         const float    boost_divisor = 66.0f;

   };

} /// namespace eosio
