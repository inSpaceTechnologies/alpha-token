/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */

#include <is.protocoin/is.protocoin.hpp>
#include <eosiolib/transaction.hpp>

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

    statstable.emplace( _self, [&]( auto& s ) {
       s.supply.symbol = maximum_supply.symbol;
       s.max_supply    = maximum_supply;
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
   eosio_assert( from.balance.amount >= value.amount, "overdrawn balance" );

   from_acnts.modify( from, owner, [&]( auto& a ) {
         a.balance -= value;
      });
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
                      uint32_t     duration )
{
    require_auth( staker );
    eosio_assert( is_account( staker ), "staker account does not exist");

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
      s.duration = duration;
   });

   int64_t weight = stake_weight(duration) * quantity.amount;

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

void token::updatestakes( const symbol& symbol ) {
   require_auth( _self );

   eosio::print("Updating stakes\n");

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
         const eosio::time_point_sec expiryTime = stk.start + stk.duration;
         if (expiryTime <= currentTime) {
            // stake has expired. remove it.
            stake_iterator = stakestable.erase(stake_iterator);
         } else {
            total_stake.amount += stk.quantity.amount;

            int64_t weight = stake_weight(stk.duration) * stk.quantity.amount;
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

   // schedule a transaction to do it again
   eosio::transaction out;
   out.actions.emplace_back(
      permission_level{_self, "active"_n},
      _self,
      "updatestakes"_n,
      std::make_tuple(symbol));
   out.delay_sec = update_interval;
   out.send(_self.value + now(), _self); // needs a unique sender id so append current time
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

} /// namespace eosio

EOSIO_DISPATCH( eosio::token, (create)(transfer)(open)(close)(addstake)(updatestakes) )
