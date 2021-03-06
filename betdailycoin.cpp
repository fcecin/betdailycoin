#include <betdailycoin.hpp>

namespace eosio {

void dice::withdraw( name to, asset quantity ) {
  require_auth( to );

  check( quantity.symbol == TOKEN_SYMBOL, "unsupported token symbol" );
  check( quantity.is_valid(), "invalid quantity" );
  check( quantity.amount > 0, "must withdraw positive quantity" );

  // subtract balance
  sub_balance( to, quantity, true );

  // withdraw tokens
  pay( to, quantity, "" );
}

void dice::commit( name host, const checksum256& commitment ) {
  require_auth( host );

  // Commitments are game proposals, and at first they are not matched to a player
  //   and to a bet size. When a player wants to play, they are matched with a
  //   commitment that belongs to a host that can cover the player bet with their
  //   current token deposit balance.

  // Check that the host has a positive deposit balance.
  // Hosts can only propose commitments AFTER they have Shown Us The Money.
  
  accounts acnts( _self, _self.value );
  auto owner_accounts = acnts.get_index<"byowner"_n>();
  auto it = owner_accounts.find( host.value );
  check( it != owner_accounts.end(), "cannot commit with a bankroll of zero" );

  // Check that the commitment's first 64 bits are unique among all commitments
  //   in the matches table.
  // That works because the matches table is a superset of the games table. When
  //   an empty game is created, a dummy clone entry is added to the match table
  //   as well (because intercepted player transfers--bets) cannot allocate RAM.
  //   So we just search the matches table here.

  uint64_t hash_prefix = get_hash_prefix( commitment );
  check( hash_prefix != ZERO_SOURCE, "A zeroed-out checksum256 is not an acceptable commitment source" );

  matches mts( _self, _self.value );
  auto mt_collision_it = mts.find( hash_prefix );
  check( mt_collision_it == mts.end(), "commitment already exists or was generated from a bad seed" );
  
  // It is unique, so create an entry for it (host pays RAM, so no other checks or limitations needed)
  games gms( _self, _self.value );
  gms.emplace( host, [&]( auto& g ){
      g.commitment = commitment;
      g.host = host;
    });

  // We need to preallocate a mirror, dummy match entry because the player won't
  //   be able to pay for RAM.
  mts.emplace( host, [&]( auto& m ){
      m.commitment   = commitment;
      m.host         = host;
      m.guess        = NULL_GUESS;
      m.bits         = 0;
      m.player       = NULL_NAME;
      m.bet          = ZERO_TOKENS;
      m.deadline     = time_point_sec(get_current_time()); // just debug info
    });
}

void dice::cancelcommit( name host, const checksum256& commitment ) {
  require_auth( host );

  // find the match
  uint64_t hash_prefix = get_hash_prefix( commitment );
  matches mts( _self, _self.value );
  auto mit = mts.find( hash_prefix );
  check( mit != mts.end(), "commitment not found" );
  const match & em = *mit;
  
  // can only cancel commitments that are not waiting for a reveal already.
  check( em.guess == NULL_GUESS, "cannot cancel commitment: already in play" );
  
  // delete match entry
  mts.erase( mit );

  // delete game entry
  games gms( _self, _self.value );
  auto git = gms.find( hash_prefix );
  gms.erase( git );
}

void dice::reveal( const checksum256& commitment, const checksum256& source ) {

  // check that the provided source and commitment parameters match 
  const auto & source_array = source.extract_as_byte_array();
  assert_sha256( (char *)&source_array[0], 32, commitment );

  // find the match
  uint64_t hash_prefix = get_hash_prefix( commitment );
  matches mts( _self, _self.value );
  auto mit = mts.find( hash_prefix );
  check( mit != mts.end(), "commitment not found" );
  const match & em = *mit;

  // Figure out who to pay for what.
  if (em.guess != NULL_GUESS) {
    
    // build the payout values. we have to remove 0.0001 token from them because we pay out
    //   0.0001 token when the player loses in order to notify them of the loss.
    // the win quantity depends on the number of bits in the guess.
    uint64_t betmult = get_mult( em.bits );
    // em.bet: player's part
    // em.bet * betmult: host's stake
    asset win_quantity = em.bet + (em.bet * betmult) - TOKEN_FRACTION;

    // check who won and issue the correct payout transaction to the player and calculate  
    //   the corresponding payout amount to the host.
    asset host_payout;
    asset player_payout;
    string player_message;

    uint64_t result = 0;
    uint64_t byteshift = 0;
    int bitsleft = em.bits;
    bool done = false;
    
    for (int si = 31 ; si >= 0 ; --si) {
      uint64_t mask = 1;
      
      for (int i = 0; i < 8; ++i) {
	
	uint64_t v = source_array[si] & mask;
	v <<= byteshift;  
	result += v;
	
	--bitsleft;
	if (bitsleft <= 0) {
	  done = true;
	  break;
	}
	
	mask <<= 1;
      }

      if (done)
	break;
      
      byteshift += 8;
    }
    
    if (result == em.guess) {
      host_payout = TOKEN_FRACTION;
      player_payout = win_quantity;
      player_message = "Win!";
    } else {
      host_payout = win_quantity;
      player_payout = TOKEN_FRACTION;
      player_message = "Loss";
    }

    // notify and/or pay player
    pay( em.player, player_payout, player_message );

    // update host balance
    add_balance( em.host, host_payout, false );

  } else {

    // This is a reveal without a player, i.e. the "match" was just the placeholder match entry
    //   that we created because we can't charge RAM to the player.
    // So this is just another way to do a cancel_commit().
    // Since the match was still open, there is a game entry that needs to be cleaned up as well.
    games gms( _self, _self.value );
    auto git = gms.find( hash_prefix );
    gms.erase( git );
  }

  // delete match entry
  mts.erase( mit );
}

void dice::collect( name player ) {

  // for every match that player is in (search byplayer)
  matches mts( _self, _self.value );
  auto player_matches = mts.get_index<"byplayer"_n>();
  auto it = player_matches.find( player.value );

  uint32_t now = get_current_time();
  
  while ( it != player_matches.end() ) {

    const match & em = *it;

    // if match has timed out
    if (now > em.deadline.sec_since_epoch()) {
      
      // send the player their winnings, which is everything (full penalty for timeouts)
      // em.bet = player's original bet
      // (em.bet * betmult) = host's stake
      uint64_t betmult = get_mult( em.bits );
      pay( player, em.bet + (em.bet * betmult), "Win! (Timeout)");

      // delete match entry & move iterator to next match entry
      it = player_matches.erase( it );
      
    } else {
      ++it;
    }
  }
}

void dice::token_transfer( name from, name to, asset quantity, string memo ) {

  // Not interested in actions where we are paying others.
  if ( from == _self )
    return;
  
  check( quantity.symbol == TOKEN_SYMBOL, "unsupported token symbol" );
  check( quantity.is_valid(), "invalid quantity" );
  check( quantity.amount >= MIN_TRANSFER_FRACTIONS, "minimum quantity not met" ); // avoid deposit spam & serves as minimum bet guard
  check( memo.size() <= 256, "memo has more than 256 bytes" );

  // If memo is exactly "deposit", this is a host funding its games.
  // If memo is exactly "odd" or "0", this is a bet for an odd number.
  // If memo is exactly "even" or "1", this is a bet for an even number.
  // If memo is anything else, the transaction is rejected.

  if (memo == "odd" || memo == "Odd" || memo == "ODD" || memo == "1") {
    do_bet(from, quantity, 1, 1);
  } else if (memo == "even" || memo == "Even" || memo == "EVEN" || memo == "0") {
    do_bet(from, quantity, 0, 1);
  } else if (memo == "deposit" || memo == "Deposit" || memo == "DEPOSIT") {
    add_balance( from, quantity, true );
  } else {
    // Try to parse exactly two positive integer numbers as arguments for the "advanced" mode,
    //   without tolerating any whitespace padding, so exactly "%i %i"
    // In the "advanced" mode, the first number is a number of bits B, and the second number is
    //   the bit pattern guess G that is the player's guess for the integer value of B bits.
    // Example: 4 5
    // The dice roll has 2 bits, so 2^4 = 16 possible values for four bits (0000 ... 1111),
    //   i.e. the positive integer value of the guess G will be between 0 and 15, and the
    //   player is betting that the resulting dice roll [0, 15] will roll a "5" (G=5).

    std::string s = memo;
    std::string delimiter = " \t\r\n";
    size_t pos = 0;
    std::string token1;
    std::string token2;
    int64_t bits = 0;
    int64_t guess = 0;
    bool ok = false;
      if ((pos = s.find(delimiter)) != std::string::npos && pos > 0) {
	token1 = s.substr(0, pos);
	s.erase(0, pos + delimiter.length());
	if ((pos = s.find(delimiter)) != std::string::npos) {	  
	  token2 = s.substr(0, pos);
	  s.erase(0, pos + delimiter.length());
	  if ((pos = s.find(delimiter)) == std::string::npos) {
	    bits = stoi(token1);
	    guess = stoi(token2);
	    if (bits >= 1 && bits <= 20) {
	      uint64_t maxguess = get_mult(bits);
	      if (guess >= 0 && guess <= maxguess) {
		ok = true;
	      }
	    }
	  }
	}
      }

    // Didn't conform to the "advanced" betting mode either
    check( ok, "memo must be: 'odd', 'even', 'deposit' or '<B=[1,20]> <G=[0,(2^B)-1]>'");

    do_bet(from, quantity, guess, bits);
  }
}

  void dice::do_bet( name player, asset quantity, uint32_t guess, uint8_t bits ) {

  // First we search for a host that has a sufficient balance to cover our bet.
  asset max_bankroll = ZERO_TOKENS;

  // host quantity at stake
  uint64_t betmult = get_mult( bits ); // 2^bits - 1
  asset host_quantity = quantity * betmult;

  accounts acnts( _self, _self.value );
  auto ait = acnts.begin();
  while (ait != acnts.end()) {

    const account & acct = *ait;

    // At most 1% of a host's current bankroll is at risk in a bet.
    if ((acct.balance / MAX_BET_TO_BANKROLL_RATIO) >= host_quantity) {

      // 1% of the acct balance can cover the bet.
      // Now let's search for any open game (commitment) that this acct hosts.
      games gms( _self, _self.value );
      auto host_games = gms.get_index<"byhost"_n>();
      auto git = host_games.find( acct.owner.value );
      if (git != host_games.end()) {
	
	// We got one free commitment.
	const game & eg = *git;
	
	// Fund the match by subtracting from the host's account balance.
	sub_balance( acct.owner, host_quantity, false );

	// Find and fill in the dummy match entry with an actual player now
	matches mts( _self, _self.value );
	auto mit = mts.find( get_hash_prefix( eg.commitment ) );
	mts.modify( mit, same_payer, [&]( auto& m ){
	    m.guess        = guess;
	    m.bits         = bits;
	    m.player       = player;
	    m.bet          = quantity;
	    m.deadline     = time_point_sec(get_current_time() + GAME_TIMEOUT_SECS);
	  });
	
	// Remove the game entry (open game offer), leaving only the ongoing,
	//   active match entry.
	host_games.erase( git );

	// And we are done.
	return;
      }
            
    } else if (acct.balance > max_bankroll) {

      // If this host has an open game offer, we can record their bankroll
      //   as the new maximum bankroll availble to cover bets.
      games gms( _self, _self.value );
      auto host_games = gms.get_index<"byhost"_n>();
      auto git = host_games.find( acct.owner.value );
      if (git != host_games.end()) {
	max_bankroll = acct.balance;
      }
    }

    ++ait;
  }

  // Did not find a single game to match this player's bet, so refuse the player's token transfer.
  max_bankroll /= MAX_BET_TO_BANKROLL_RATIO; // Max bet is actually 1% of the max bankroll.
  if (max_bankroll.amount < MIN_TRANSFER_FRACTIONS) {
    check( false, "no bets available" );
  } else {
    string msg = "the current maximum bet is ";
    msg.append( max_bankroll.to_string() );
    check( false, msg );
  }
}

void dice::pay( name to, asset quantity, string memo ) {
  action(
	 permission_level{ _self, "active"_n },
	 TOKEN_CONTRACT, "transfer"_n,
	 std::make_tuple(_self, to, quantity, memo)
	 ).send();
}

void dice::add_balance( name owner, asset value, bool enforce_min ) {
  accounts acnts( _self, _self.value );
  auto owner_accounts = acnts.get_index<"byowner"_n>();
  auto it = owner_accounts.find( owner.value );
  if( it == owner_accounts.end() ) {

    if (enforce_min) {
      // Enforce a minimum balance to allow the creation of an account.
      // This helps because players iterate over all accounts to find a suitable game host.
      check( value >= MIN_BALANCE, "deposit does not meet minimum balance requirement" ); 
    }

    // The RAM payer needs to be the contract itself. We can't charge RAM during
    //   an incoming token transfer.
    // (Not sure how this was working at all before, with "owner" being charged
    //   for the RAM.)
    // The only other way to do this is create an "open account" action that charges
    //   RAM to the caller. For now let's just do this.
    acnts.emplace( _self, [&]( auto& a ){
	a.owner   = owner;
	a.balance = value;
      });
  } else {
    owner_accounts.modify( it, same_payer, [&]( auto& a ) {
	a.balance += value;
      });
  }
}

void dice::sub_balance( name owner, asset value, bool enforce_min ) {
  accounts acnts( _self, _self.value );
  auto owner_accounts = acnts.get_index<"byowner"_n>();
  auto it = owner_accounts.find( owner.value );
  check( it != owner_accounts.end(), "no account object found" );

  const auto& owner_account = *it;
  check( owner_account.balance.amount >= value.amount, "overdrawn balance" );

  asset result = owner_account.balance - value;
  if (result.amount == 0) {
    owner_accounts.erase( it );

    // Wiping your host account balance clean is an implicit request to cancel every single game
    //   offer that has not been taken yet.
    matches mts( _self, _self.value );
    games gms( _self, _self.value );
    auto host_games = gms.get_index<"byhost"_n>();
    auto git = host_games.find( owner.value );
    while (git != host_games.end()) {
      auto mit = mts.find( git->primary_key() ); // any match that has a corresponding game is by definition empty
      mts.erase( mit );                          // so just erase it, no need to test mit->guess == NULL_GUESS (it is.)
      git = host_games.erase( git );
    }

  } else {

    if (enforce_min) {
      // When withdrawing tokens to an external account, either you are withdrawing everything, or
      //   you need to leave a minimum balance, in order to prevent host-balance (account entry) spam.
      check( result >= MIN_BALANCE,
	     "withdrawal must either withdraw the full balance, or the remainder must meet the minimum balance requirement" );

      // In addition, you cannot withdraw less than the minimum transfer amount if you're not
      //   emptying the account.
      check( value.amount >= MIN_TRANSFER_FRACTIONS, "withdrawals below the minimum transfer are only allowed when emptying the account" ); 
    }

    owner_accounts.modify( it, same_payer, [&]( auto& a ) {
	a.balance = result;
      });
  }
}

extern "C" {
  void apply(uint64_t receiver, uint64_t code, uint64_t action) {
    if (code == dice::TOKEN_CONTRACT.value && action == "transfer"_n.value) {
      eosio::execute_action(eosio::name(receiver), eosio::name(code), &dice::token_transfer);
    } else if (code == receiver) {
      switch (action) { EOSIO_DISPATCH_HELPER(dice, (withdraw)(commit)(cancelcommit)(reveal)(collect)) }
    }
    eosio_exit(0);
  }
}

} /// namespace eosio
