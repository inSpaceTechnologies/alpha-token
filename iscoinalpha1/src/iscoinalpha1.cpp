/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */

#include <iscoinalpha1/iscoinalpha1.hpp>
#include <eosiolib/transaction.hpp>
#include <math.h> /* exp */

namespace eosio {

void token::create( asset  maximum_supply )
{
    require_auth( _self );

    auto sym = maximum_supply.symbol;
    eosio_assert( sym.is_valid(), "invalid symbol name" );
    eosio_assert( maximum_supply.is_valid(), "invalid supply");
    eosio_assert( maximum_supply.amount > 0, "max-supply must be positive");

    stats statstable( _self, sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    eosio_assert( existing == statstable.end(), "token with symbol already exists" );

    eosio::time_point_sec current_time = eosio::time_point_sec(now());

    statstable.emplace( _self, [&]( auto& s ) {
       s.supply.symbol = maximum_supply.symbol;
       s.max_supply    = maximum_supply;
       s.created       = current_time;
       s.updated       = current_time;
       s.boosts        = 0;
    });

    const int64_t issue_amount = (int64_t)(maximum_supply.amount * ISSUE_PROPORTION);
    issue(asset(issue_amount, sym));
}

void token::transfer( name    from,
                      name    to,
                      asset   quantity,
                      string  memo )
{
    eosio_assert( from != to, "cannot transfer to self" );
    require_auth( from );
    eosio_assert( is_account( to ), "to account does not exist");
    auto sym = quantity.symbol.code();
    stats statstable( _self, sym.raw() );
    const auto& st = statstable.get( sym.raw() );

    require_recipient( from );
    require_recipient( to );

    eosio_assert( quantity.is_valid(), "invalid quantity" );
    eosio_assert( quantity.amount > 0, "must transfer positive quantity" );
    eosio_assert( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    eosio_assert( memo.size() <= 256, "memo has more than 256 bytes" );

    auto payer = has_auth( to ) ? to : from;

    sub_balance( from, quantity );
    add_balance( to, quantity, payer );
}

void token::transferstkd( name    from,
                   name    to,
                   asset   quantity,
                   string  memo,
                   size_t   duration_index )
{
   SEND_INLINE_ACTION( *this, transfer, { {from, "active"_n} },
                       { from, to, quantity, memo }
   );
   // can't use the addstake action, because we don't have the authority
   add_stake(to, quantity, duration_index);
}

void token::issue( asset quantity )
{
    auto sym = quantity.symbol;
    eosio_assert( sym.is_valid(), "invalid symbol name" );

    stats statstable( _self, sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    eosio_assert( existing != statstable.end(), "token with symbol does not exist, create token before issue" );
    const auto& st = *existing;

    eosio_assert( quantity.is_valid(), "invalid quantity" );
    eosio_assert( quantity.amount > 0, "must issue positive quantity" );

    eosio_assert( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    eosio_assert( quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

    statstable.modify( st, same_payer, [&]( auto& s ) {
       s.supply += quantity;
    });

    add_balance( _self, quantity, _self );
}

void token::sub_balance( name owner, asset value ) {
   accounts from_acnts( _self, owner.value );

   const auto& from = from_acnts.get( value.symbol.code().raw(), "no balance object found" );

   const asset stake = get_stake(owner, value.symbol );

   const int64_t transaction_fee_amount = (int64_t)(value.amount * transaction_fee);
   const int64_t total_amount = value.amount + transaction_fee_amount;

   eosio_assert( from.balance.amount - stake.amount >= total_amount, "overdrawn unstaked balance" );

   from_acnts.modify( from, owner, [&]( auto& a ) {
         a.balance.amount -= total_amount;
      });

   int64_t transaction_fee_remaining = transaction_fee_amount;
   const int64_t transaction_fee_stakers_amount = (int64_t)(transaction_fee_to_stakers * transaction_fee_amount);
   asset transaction_fee_stakers_asset(transaction_fee_stakers_amount, value.symbol);

   transaction_fee_remaining -= distribute(transaction_fee_stakers_asset);

   if (transaction_fee_remaining > 0) {
      asset transaction_fee_inspace_asset(transaction_fee_remaining, value.symbol);
      add_balance(_self, transaction_fee_inspace_asset, _self);
   }
}

void token::add_balance( name owner, asset value, name ram_payer )
{
   accounts to_acnts( _self, owner.value );
   auto to = to_acnts.find( value.symbol.code().raw() );
   if( to == to_acnts.end() ) {
      to_acnts.emplace( ram_payer, [&]( auto& a ){
        a.balance = value;
      });
   } else {
      to_acnts.modify( to, same_payer, [&]( auto& a ) {
        a.balance += value;
      });
   }
}

void token::open( name owner, const symbol& symbol, name ram_payer )
{
   require_auth( ram_payer );

   auto sym_code_raw = symbol.code().raw();

   stats statstable( _self, sym_code_raw );
   const auto& st = statstable.get( sym_code_raw, "symbol does not exist" );
   eosio_assert( st.supply.symbol == symbol, "symbol precision mismatch" );

   accounts acnts( _self, owner.value );
   auto it = acnts.find( sym_code_raw );
   if( it == acnts.end() ) {
      acnts.emplace( ram_payer, [&]( auto& a ){
        a.balance = asset{0, symbol};
      });
   }
}

void token::close( name owner, const symbol& symbol )
{
   require_auth( owner );
   accounts acnts( _self, owner.value );
   auto it = acnts.find( symbol.code().raw() );
   eosio_assert( it != acnts.end(), "Balance row already deleted or never existed. Action won't have any effect." );
   eosio_assert( it->balance.amount == 0, "Cannot close because the balance is not zero." );
   acnts.erase( it );
}

void token::addstake( name         staker,
                      asset        quantity,
                      size_t       duration_index )
{
    require_auth( staker );
    add_stake(staker, quantity, duration_index);
}

void token::add_stake( name         staker,
                      asset        quantity,
                      size_t       duration_index )
{
    eosio_assert( is_account( staker ), "staker account does not exist");

    eosio_assert( duration_index < stake_count, "duration_index out of bounds");

    stats statstable( _self, quantity.symbol.code().raw() );
    const auto& st = statstable.get( quantity.symbol.code().raw() );

    eosio_assert( quantity.is_valid(), "invalid quantity" );
    eosio_assert( quantity.amount > 0, "must stake positive quantity" );
    eosio_assert( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );

    const asset unstaked_balance = get_unstaked_balance(staker, quantity.symbol);
    eosio_assert( quantity.amount <= unstaked_balance.amount, "overdrawn unstaked balance" );

    stakes staker_stakes( _self, staker.value );
    staker_stakes.emplace(_self, [&](auto& s) {
      s.id = staker_stakes.available_primary_key();
      s.quantity = quantity;
      s.start = eosio::time_point_sec(now());
      s.duration_index = duration_index;
   });

   int64_t weight = stake_weights[duration_index];

   stake_stats stake_stats_table( _self, quantity.symbol.code().raw() );
   const auto staker_stake_stats = stake_stats_table.find( staker.value );
   if( staker_stake_stats == stake_stats_table.end() ) {
      stake_stats_table.emplace( _self, [&]( auto& s ){
         s.staker = staker;
         s.total_stake = quantity;
         s.stake_weight = weight;
      });
   } else {
      stake_stats_table.modify( staker_stake_stats, _self, [&]( auto& s ) {
         s.total_stake += quantity;
         s.stake_weight += weight;
      });
   }
}

void token::update( const symbol& symbol ) {
   require_auth( _self );

   eosio::print("Updating\n");

   eosio_assert( symbol.is_valid(), "invalid symbol name" );

   update_stakes(symbol);
   update_boost(symbol);

   // schedule a transaction to do it again
   eosio::transaction out;
   out.actions.emplace_back(
      permission_level{_self, "active"_n},
      _self,
      "update"_n,
      std::make_tuple(symbol));
   out.delay_sec = update_interval;
   out.send(_self.value + now(), _self); // needs a unique sender id so append current time
}

void token::update_stakes( const symbol& symbol ) {

   stake_stats stake_stats_table( _self, symbol.code().raw() );

   // iterate through stake stats
   // (all stakes will have an entry because addstake adds one)
   auto iterator = stake_stats_table.begin();
   while ( iterator != stake_stats_table.end() ) {

      const auto& st = (*iterator);
      // iterate through the staker's stakes
      stakes stakestable( _self, st.staker.value );

      asset total_stake(0, symbol);

      int64_t this_stake_weight = 0;

      const eosio::time_point_sec currentTime(now());
      auto stake_iterator = stakestable.begin();
      while(stake_iterator != stakestable.end()) {
         const auto& stk = (*stake_iterator);
         if (stk.quantity.symbol != symbol) {
            ++stake_iterator;
            continue;
         }
         const uint32_t duration = stake_durations[stk.duration_index];
         const eosio::time_point_sec expiryTime = stk.start + duration;
         if (expiryTime <= currentTime) {
            // stake has expired. remove it.
            stake_iterator = stakestable.erase(stake_iterator);
         } else {
            total_stake.amount += stk.quantity.amount;

            int64_t weight = stake_weights[stk.duration_index] * stk.quantity.amount;
            this_stake_weight += weight;

            ++stake_iterator;
         }
      }

      if (total_stake.amount == 0) {
         // all stakes have expired.
         // remove entry
         iterator = stake_stats_table.erase(iterator);
      } else {
         // update stake stats
         stake_stats_table.modify( iterator, _self, [&]( auto& s ) {
            s.total_stake = total_stake;
            s.stake_weight = this_stake_weight;
         });
         ++iterator;
      }
   }
}


void token::update_boost( const symbol& symbol ) {
   require_auth( _self );

   eosio::print("Updating boost.\n");

   stats statstable( _self, symbol.code().raw() );
   auto existing = statstable.find( symbol.code().raw() );
   eosio_assert( existing != statstable.end(), "token with symbol does not exist." );
   const auto& st = *existing;

   const eosio::time_point_sec current_time(now());
   eosio::print("Current time:", current_time.sec_since_epoch(), "\n");

   const uint16_t next_boost = st.boosts + 1;
   eosio::print("Current boost:", (uint32_t)st.boosts, "\n");
   eosio::print("Next boost:", (uint32_t)next_boost, "\n");

   if (next_boost > boost_count) {
      // no more boosts
      return;
   }

   const eosio::time_point_sec next_boost_time = st.created + next_boost * boost_interval;
   eosio::print("Next boost time:", next_boost_time.sec_since_epoch(), "\n");

   if (next_boost_time <= current_time) {
      // it's time for the next boost

      const int64_t total_boost = (int64_t)(boost_proportion() * st.max_supply.amount);
      eosio::print("Total boost:", total_boost, "\n");
      const int64_t current_boost_amount = (exp(boost_lambda*next_boost)/boost_divisor) * total_boost;
      eosio::print("Current boost:", current_boost_amount, "\n");
      const asset current_boost_asset(current_boost_amount, symbol);

      if ( st.supply.amount + current_boost_asset.amount > st.max_supply.amount) {
         // not enough supply
         return;
      }

      statstable.modify( st, same_payer, [&]( auto& s ) {
         s.supply += current_boost_asset;
         s.updated = current_time;
         s.boosts = next_boost;
      });

      int64_t amount_distributed = distribute(current_boost_asset);
      eosio::print("Amount distributed:", amount_distributed, "\n");
      // give remainder to this account
      int64_t remainder = current_boost_asset.amount - amount_distributed;
      eosio::print("Remainder:", remainder, "\n");
      if (remainder > 0) {
         add_balance( _self, asset(remainder, symbol), _self);
      }
   }
}

asset token::get_stake( name staker, const symbol& symbol )const
{
   stake_stats stake_stats_table( _self, symbol.code().raw() );
   const auto staker_stake_stats = stake_stats_table.find( staker.value );
   if( staker_stake_stats == stake_stats_table.end() ) {
      // no enty, so no stakes
      asset ret(0, symbol);
      return ret;
   } else {
      return (*staker_stake_stats).total_stake;
   }
}

int64_t token::get_stake_weight( name staker, const symbol& symbol )const
{
   stake_stats stake_stats_table( _self, symbol.code().raw() );
   const auto staker_stake_stats = stake_stats_table.find( staker.value );
   if( staker_stake_stats == stake_stats_table.end() ) {
      // no enty, so no stakes
      return (int64_t)0;
   } else {
      return (*staker_stake_stats).stake_weight;
   }
}

asset token::get_unstaked_balance( name owner, const symbol& symbol )const
{
   const asset balance = get_balance(_self, owner, symbol.code());
   const asset stake = get_stake(owner, symbol);
   return asset(balance.amount - stake.amount, symbol);
}

// distributes the quantity amongst stakers by stake weight.
// returns the actual amount distruted.
int64_t token::distribute( asset quantity )
{
   eosio::print("Distributing:", quantity.amount, "\n");

   stake_stats stake_stats_table( _self, quantity.symbol.code().raw() );

   std::vector<name>  stakers;
   std::vector<int64_t>       weights;
   int64_t                    total_weight = 0;

   // iterate through stake stats
   auto iterator = stake_stats_table.begin();
   while ( iterator != stake_stats_table.end() ) {

      const auto& st = (*iterator);

      stakers.push_back(st.staker);
      weights.push_back(st.stake_weight);
      total_weight += st.stake_weight;

      ++iterator;
   }

   if (total_weight == 0) {
      return 0;
   }

   int64_t amount_distributed = 0;

   for(size_t i = 0; i < stakers.size(); i++) {
      name staker = stakers[i];

      int64_t staker_weight = weights[i];

      float proportion = (float)staker_weight / total_weight;

      int64_t amount_for_staker = (int64_t)(quantity.amount  * proportion);

      asset amount_asset;
      amount_asset.symbol = quantity.symbol;
      amount_asset.amount = amount_for_staker;

      add_balance( staker, amount_asset, _self);
      amount_distributed += amount_for_staker;
   }

   return amount_distributed;
}

} /// namespace eosio

EOSIO_DISPATCH( eosio::token, (create)(transfer)(transferstkd)(open)(close)(addstake)(update) )
