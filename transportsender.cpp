#include "transportsender.hpp"
#include "transportfragment.hpp"

using namespace Network;

template <class MyState>
TransportSender<MyState>::TransportSender( Connection *s_connection, MyState &initial_state )
  : connection( s_connection ), 
    current_state( initial_state ),
    sent_states( 1, TimestampedState<MyState>( timestamp(), 0, initial_state ) ),
    assumed_receiver_state( sent_states.begin() ),
    next_instruction_id( -1 ),
    last_instruction_sent(),
    next_ack_time( timestamp() ),
    next_send_time( timestamp() ),
    verbose( false ),
    ack_num( 0 )
{
}

/* Try to send roughly two frames per RTT, bounded by limits on frame rate */
template <class MyState>
unsigned int TransportSender<MyState>::send_interval( void )
{
  int SEND_INTERVAL = lrint( ceil( (connection->get_SRTT() - ACK_DELAY) / 2.0 ) );
  if ( SEND_INTERVAL < SEND_INTERVAL_MIN ) {
    SEND_INTERVAL = SEND_INTERVAL_MIN;
  } else if ( SEND_INTERVAL > SEND_INTERVAL_MAX ) {
    SEND_INTERVAL = SEND_INTERVAL_MAX;
  }

  return SEND_INTERVAL;
}

/* How many ms can the caller wait before we will have an event (empty ack or next frame)? */
template <class MyState>
int TransportSender<MyState>::wait_time( void )
{
  if ( connection->pending_timestamp() && ( next_ack_time > timestamp() + ACK_DELAY ) ) {
    next_ack_time = timestamp() + ACK_DELAY;
  }

  uint64_t next_wakeup = next_ack_time;

  if ( !(current_state == sent_states.back().state) ) { /* pending data to send */
    if ( next_send_time > timestamp() + SEND_MINDELAY ) {
      next_send_time = timestamp() + SEND_MINDELAY;
    }

    if ( next_send_time < sent_states.back().timestamp + send_interval() ) {
      next_send_time = sent_states.back().timestamp + send_interval();
    }

    if ( next_send_time < next_wakeup ) {
      next_wakeup = next_send_time;
    }
  }

  if ( !connection->get_attached() ) {
    return -1;
  }

  if ( next_wakeup > timestamp() ) {
    return next_wakeup - timestamp();
  } else {
    return 0;
  }
}

/* Send data or an empty ack if necessary */
template <class MyState>
void TransportSender<MyState>::tick( void )
{
  wait_time();

  if ( !connection->get_attached() ) {
    return;
  }

  if ( (timestamp() < next_ack_time)
       && (timestamp() < next_send_time) ) {
    return;
  }

  /* Determine if a new diff or empty ack needs to be sent */
  /* Update assumed receiver state */
  update_assumed_receiver_state();
    
  /* Cut out common prefix of all states */
  rationalize_states();

  string diff = current_state.diff_from( assumed_receiver_state->state );

  if ( diff.empty() && (timestamp() >= next_ack_time) ) {
    send_empty_ack();
    return;
  }

  if ( !diff.empty() && ( (timestamp() >= next_send_time)
			  || (timestamp() >= next_ack_time) ) ) {
    /* Send diffs or ack */
    send_to_receiver( diff );
  }
}

template <class MyState>
void TransportSender<MyState>::send_empty_ack( void )
{
  assert ( timestamp() >= next_ack_time );

  uint64_t new_num = sent_states.back().num + 1;

  send_in_fragments( "", new_num, false );
  sent_states.push_back( TimestampedState<MyState>( sent_states.back().timestamp, new_num, current_state ) );
  next_ack_time = timestamp() + ACK_INTERVAL;
}

template <class MyState>
void TransportSender<MyState>::send_to_receiver( string diff )
{
  uint64_t new_num;
  if ( current_state == sent_states.back().state ) { /* previously sent */
    new_num = sent_states.back().num;
  } else { /* new state */
    new_num = sent_states.back().num + 1;
  }

  bool done = false;
  int MTU_tries = 0;
  while ( !done ) {
    MTU_tries++;

    if ( MTU_tries > 20 ) {
      fprintf( stderr, "Error, could not send fragments after 20 tries (MTU = %d).\n",
	       connection->get_MTU() );
    }

    try {
      send_in_fragments( diff, new_num );
      done = true;
    } catch ( MTUException m ) {
      fprintf( stderr, "Caught Path MTU exception, MTU now = %d\n", connection->get_MTU() );
      done = false;
    }

    if ( new_num == sent_states.back().num ) {
      sent_states.back().timestamp = timestamp();
    } else {
      sent_states.push_back( TimestampedState<MyState>( timestamp(), new_num, current_state ) );
    }

    new_num++;
  }

  /* successfully sent, probably */
  /* ("probably" because the FIRST size-exceeded datagram doesn't get an error) */
  assumed_receiver_state = sent_states.end();
  assumed_receiver_state--;
  next_ack_time = timestamp() + ACK_INTERVAL;
  next_send_time = -1;
}

template <class MyState>
void TransportSender<MyState>::update_assumed_receiver_state( void )
{
  uint64_t now = timestamp();

  /* start from what is known and give benefit of the doubt to unacknowledged states
     transmitted recently enough ago */
  assumed_receiver_state = sent_states.begin();

  typename list< TimestampedState<MyState> >::iterator i = sent_states.begin();
  i++;

  while ( i != sent_states.end() ) {
    assert( now >= i->timestamp );

    if ( int(now - i->timestamp) < connection->timeout() + ACK_DELAY ) {
      assumed_receiver_state = i;
    } else {
      return;
    }

    i++;
  }
}

template <class MyState>
void TransportSender<MyState>::rationalize_states( void )
{
  const MyState * known_receiver_state = &sent_states.front().state;

  current_state.subtract( known_receiver_state );

  for ( typename list< TimestampedState<MyState> >::reverse_iterator i = sent_states.rbegin();
	i != sent_states.rend();
	i++ ) {
    i->state.subtract( known_receiver_state );
  }
}

template <class MyState>
void TransportSender<MyState>::send_in_fragments( string diff, uint64_t new_num, bool send_timestamp )
{
  Instruction inst;
  inst.set_old_num( assumed_receiver_state->num );
  inst.set_new_num( new_num );
  inst.set_ack_num( ack_num );
  inst.set_throwaway_num( sent_states.front().num );
  inst.set_diff( diff );

  if ( (inst.old_num() != last_instruction_sent.old_num())
       || (inst.new_num() != last_instruction_sent.new_num())
       || (inst.ack_num() != last_instruction_sent.ack_num())
       || (inst.throwaway_num() != last_instruction_sent.throwaway_num()) ) {
    next_instruction_id++; /* make sure this happens before the first send in case of exception */
    last_instruction_sent = inst;
  } else {
    assert( inst.diff() == last_instruction_sent.diff() );
  }

  string payload = inst.SerializeAsString();

  uint16_t fragment_num = 0;

  do {
    string this_fragment;
    
    assert( fragment_num <= 32767 );

    bool final = false;

    int MTU = connection->get_MTU();

    if ( int( payload.size() + HEADER_LEN ) > MTU ) {
      this_fragment = string( payload.begin(), payload.begin() + MTU - HEADER_LEN );
      payload = string( payload.begin() + MTU - HEADER_LEN, payload.end() );
    } else {
      this_fragment = payload;
      payload.clear();
      final = true;
    }

    Fragment frag( next_instruction_id, fragment_num++, final, this_fragment );
    string s = frag.tostring();

    connection->send( s, send_timestamp );

    if ( verbose ) {
      fprintf( stderr, "[%d] Sent [%d=>%d] id %d, frag %d ack=%d, throwaway=%d, len=%d, frame rate=%.2f, timeout=%d\n",
	       (int)(timestamp() % 100000), (int)inst.old_num(), (int)inst.new_num(), (int)frag.id, (int)frag.fragment_num,
	       (int)inst.ack_num(), (int)inst.throwaway_num(), (int)frag.contents.size(),
	       1000.0 / (double)send_interval(),
	       (int)connection->timeout() );
    }
  } while ( !payload.empty() );
}

template <class MyState>
void TransportSender<MyState>::process_acknowledgment_through( uint64_t ack_num )
{
  typename list< TimestampedState<MyState> >::iterator i = sent_states.begin();
  while ( i != sent_states.end() ) {
    typename list< TimestampedState<MyState> >::iterator inext = i;
    inext++;
    if ( i->num < ack_num ) {
      sent_states.erase( i );
    }
    i = inext;
  }

  assert( sent_states.size() > 0 );
}
