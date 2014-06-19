/**
 * ringbufferbase.tcc - 
 * @author: Jonathan Beard
 * @version: Thu May 15 09:06:52 2014
 * 
 * Copyright 2014 Jonathan Beard
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef _RINGBUFFERBASE_TCC_
#define _RINGBUFFERBASE_TCC_  1

#include <array>
#include <cstdlib>
#include <cassert>
#include <thread>
#include <cstring>
#include <iostream>
#include "Clock.hpp"
#include "pointer.hpp"
#include "ringbuffertypes.hpp"
#include "bufferdata.tcc"
#include "signalvars.hpp"

/**
 * Note: there is a NICE define that can be uncommented
 * below if you want sched_yield called when waiting for
 * writes or blocking for space, otherwise blocking will
 * actively spin while waiting.
 */
#define NICE 1

extern Clock *system_clock;

typedef std::uint32_t blocked_part_t;
typedef std::uint64_t blocked_whole_t;

typedef struct {
     uint32_t
         CF      :  1,
                 :  1,
         PF      :  1,
                 :  1,
         AF      :  1,
                 :  1,
         ZF      :  1,
         SF      :  1,
         TF      :  1,
         IF      :  1,
         DF      :  1,
         OF      :  1,
         IOPL    :  2,
         NT      :  1,
                 :  1,
         RF      :  1,
         VM      :  1,
         AC      :  1,
         VIF     :  1,
         VIP     :  1,
         ID      :  1,
                 : 10;
} EFlags;


typedef struct {
   uint32_t eax, ebx, ecx, edx;
} Reg;

enum feature_levels {
	FL_NONE,
	FL_MMX,
	FL_SSE2,
	FL_AVX
};


#define    CPUID_BASIC     0x0
#define    CPUID_LEVEL1    0x1

void zero_registers (Reg *in)
{
	in->eax = 0x0;
	in->ebx = 0x0;
	in->ecx = 0x0;
	in->edx = 0x0;
}


/**
 * get_cpuid - sets eax, ebx, ecx, edx values in struct Reg based on input_eax input
 */
void get_cpuid (Reg *input_registers, Reg *output_registers)
{
#if( __i386__ == 1 || __x86_64 == 1 )
   __asm__ volatile ("\
      movl  %[input_eax], %%eax     \n\
      movl  %[input_ecx], %%ecx     \n\
      cpuid                         \n\
      movl  %%eax,        %[eax]    \n\
      movl  %%ebx,        %[ebx]    \n\
      movl  %%ecx,        %[ecx]    \n\
      movl  %%edx,        %[edx]"
      :
      [eax] "=r"  (output_registers->eax),
      [ebx] "=r"  (output_registers->ebx),
      [ecx] "=r"  (output_registers->ecx),
      [edx] "=r"  (output_registers->edx)
      :
      [input_eax] "m" (input_registers->eax),
      [input_ecx] "m" (input_registers->ecx)
      :
      "eax","ebx","ecx","edx"
      );
#endif
}

int get_level0_data (unsigned int *max_level)
{
	Reg in, out;

	in.eax = CPUID_BASIC;
	get_cpuid(&in, &out);
	
	if (max_level)
		*max_level = out.eax;
		
	return 0;
}


int get_level1_data (unsigned int max_level, unsigned int *eax, 
	unsigned int *ecx, unsigned int *edx)
{
	Reg in, out;

	in.eax = CPUID_LEVEL1;
	get_cpuid(&in, &out);
	
	if (eax) *eax = out.eax;
	if (ecx) *ecx = out.ecx;
	if (edx) *edx = out.edx;
	
	return 0;
}

enum feature_levels get_highest_feature (unsigned int max_level)
{
	unsigned int ecx, edx;

	if (max_level < 1) {
		fprintf(stderr, "Error calling cpuid, cpuid not supported\n");
		exit(-1);
	}
	
	get_level1_data(max_level, NULL, &ecx, &edx);

	if (ecx & (1 << 28))
		return FL_AVX;

	if (edx & (1 << 26))
		return FL_SSE2;
		
	if (edx & (1 << 23))
		return FL_MMX;
			
	return FL_NONE;
}

      /*  	loop128%=:				\n\
			vmovdqu (%%rax), %%ymm0		\n\
			vmovdqu 32(%%rax), %%ymm1	\n\
			vmovdqu 64(%%rax), %%ymm2	\n\
			vmovdqu 96(%%rax), %%ymm3	\n\
			vmovdqu %%ymm0, (%%rbx)		\n\
			vmovdqu %%ymm1, 32(%%rbx)	\n\
			vmovdqu %%ymm2, 64(%%rbx)	\n\
			vmovdqu %%ymm3, 96(%%rbx)	\n\
			addq	$128, %%rax		\n\
			addq	$128, %%rbx		\n\
			subq	$128, %[SIZE]		\n\
			l128ctl%=:			\n\
			cmpq	$128, %[SIZE]		\n\
			jge	loop128%=		\n\
			jmp	l8ctl%=			\n\*/

inline void rb_write (unsigned char *dstp, unsigned char *srcp, 
	size_t size, char feature_level)
{
	__asm__ volatile("\
			movq	%[in], %%rax		\n\
			movq	%[out], %%rbx		\n\
			cmpb	$1, %[fl]		\n\
			jl	l8ctl%=			\n\
			je	l32ctl%=		\n\
			jmp 	l64ctl%=		\n\
		loop64%=:				\n\
			movdqu	(%%rax), %%xmm0		\n\
			movdqu	16(%%rax), %%xmm1	\n\
			movdqu	32(%%rax), %%xmm2	\n\
			movdqu	48(%%rax), %%xmm3	\n\
			movntdq	%%xmm0, (%%rbx)		\n\
			movntdq	%%xmm1, 16(%%rbx)	\n\
			movntdq	%%xmm2, 32(%%rbx)	\n\
			movntdq	%%xmm3, 48(%%rbx)	\n\
			addq	$64, %%rax		\n\
			addq	$64, %%rbx		\n\
			subq	$64, %[SIZE]		\n\
			l64ctl%=:			\n\
			cmpq	$64, %[SIZE]		\n\
			jge	loop64%=		\n\
			jmp	l32ctl%=		\n\
		loop32%=:				\n\
			movq	(%%rax), %%mm0		\n\
			movq	8(%%rax), %%mm1		\n\
			movq	16(%%rax), %%mm2	\n\
			movq	24(%%rax), %%mm3	\n\
			movntq	%%mm0, (%%rbx)		\n\
			movntq	%%mm1, 8(%%rbx)		\n\
			movntq	%%mm2, 16(%%rbx)	\n\
			movntq	%%mm3, 24(%%rbx)	\n\
			addq	$32, %%rax		\n\
			addq	$32, %%rbx		\n\
			subq	$32, %[SIZE]		\n\
			l32ctl%=:			\n\
			cmpq	$32, %[SIZE]		\n\
			jge	loop32%=		\n\
			jmp 	l8ctl%=			\n\
		loop8%=:				\n\
			movq	(%%rax), %%rcx		\n\
			movq	%%rcx, (%%rbx)		\n\
			addq	$8, %%rax		\n\
			addq	$8, %%rbx		\n\
			subq	$8, %[SIZE]		\n\
			l8ctl%=:			\n\
			cmpq	$8, %[SIZE]		\n\
			jge	loop8%=			\n\
			jmp 	l1ctl%=			\n\
		loop1%=:				\n\
			movb	(%%rax), %%cl		\n\
			movb	%%cl, (%%rbx)		\n\
			incq	%%rax			\n\
			incq	%%rbx			\n\
			decq	%[SIZE]			\n\
			l1ctl%=:			\n\
			cmpq	$1, %[SIZE]		\n\
			jge	loop1%=			\n\
			mfence"
			:
			:
			[in] "g" (srcp), 
			[out] "g" (dstp),			
			[SIZE] "m" (size),
			[fl] "g" (feature_level)
			:
			"mm0", "mm1", "mm2", "mm3", 
			"xmm0", "xmm1", "xmm2", "xmm3",
			"rax", "rbx", "rcx");	
}

/**
 * Blocked - simple data structure to combine the send count
 * and blocked flag in one simple structure.  Greatly improves
 * synchronization between cores.
 */
union Blocked{
   
   Blocked() : all( 0 )
   {}

   Blocked( volatile Blocked &other )
   {
      all = other.all;
   }

   struct{
      blocked_part_t  count;
      blocked_part_t  blocked;
   };
   blocked_whole_t    all;
} __attribute__ ((aligned( 8 )));


template < class T, 
           RingBufferType type > class RingBufferBase {
public:
   /**
    * RingBuffer - default constructor, initializes basic
    * data structures.
    */
   RingBufferBase() : data( nullptr ),
   		      feature_level( 0 ),
                      allocate_called( false )
   {
#if(__i386__ == 1 || __x86_64 == 1)
	/** set cpuid feature level **/
	unsigned int max_level;
	if (get_level0_data(&max_level) == -1) 
	{
	   fprintf(stderr, "error getting level 0 data\n");
	}
	feature_level = get_highest_feature( max_level );
#endif
   }
   
   virtual ~RingBufferBase()
   {
   }


   /**
    * size - as you'd expect it returns the number of 
    * items currently in the queue.
    * @return size_t
    */
   size_t   size()
   {
      const auto   wrap_write( Pointer::wrapIndicator( data->write_pt  ) ),
                   wrap_read(  Pointer::wrapIndicator( data->read_pt   ) );

      const auto   wpt( Pointer::val( data->write_pt ) ), 
                   rpt( Pointer::val( data->read_pt  ) );
      if( wpt == rpt )
      {
         if( wrap_read < wrap_write )
         {
            return( data->max_cap );
         }
         else if( wrap_read > wrap_write )
         {
            /**
             * TODO, this condition is momentary, however there
             * is a better way to fix this with atomic operations...
             * or on second thought benchmarking shows the atomic
             * operations slows the queue down drastically so, perhaps
             * this is in fact the best of all possible returns.
             */
            return( data->max_cap  );
         }
         else
         {
            return( 0 );
         }
      }
      else if( rpt < wpt )
      {
         return( wpt - rpt );
      }
      else if( rpt > wpt )
      {
         return( data->max_cap - rpt + wpt ); 
      }
      return( 0 );
   }

   /**
    * send_signal - sends an asynchronous signal to the receiver.
    * @param   sig - const RBSignal
    */
   void  send_signal( const RBSignal sig )
   {
      signal_mask = sig;
   }
   
   /**
    * get_signal - returns a reference to the signal mask for this
    * queue.
    * @return volatile RBSignal&
    */
   RBSignal get_signal()
   {
      const auto out( signal_mask );
      //signal_mask = RBSignal::NONE;
      return( out ); 
   }
   
   /**
    * space_avail - returns the amount of space currently
    * available in the queue.  This is the amount a user
    * can expect to write without blocking
    * @return  size_t
    */
    size_t   space_avail()
   {
      return( data->max_cap - size() );
   }
  
   /**
    * capacity - returns the capacity of this queue which is 
    * set at compile time by the constructor.
    * @return size_t
    */
   size_t   capacity() const
   {
      return( data->max_cap );
   }

   /**
    * allocate - get a reference to an object of type T at the 
    * end of the queue.  Should be released to the queue using
    * the push command once the calling thread is done with the 
    * memory.
    * @return T&, reference to memory location at head of queue
    */
   T& allocate()
   {
      while( space_avail() == 0 )
      {
#ifdef NICE      
         std::this_thread::yield();
#endif         
         if( write_stats.blocked == 0 )
         {   
            write_stats.blocked = 1;
         }
#if __x86_64 
         __asm__ volatile("\
           pause"
           :
           :
           : );
#endif           
      }
      (this)->allocate_called = true;
      const size_t write_index( Pointer::val( data->write_pt ) );
      return( data->store[ write_index ].item );
   }

   /**
    * push - releases the last item allocated by allocate() to
    * the queue.  Function will imply return if allocate wasn't
    * called prior to calling this function.
    * @param signal - const RBSignal signal, default: NONE
    */
   void push( const RBSignal signal = RBSignal::NONE )
   {
      if( ! (this)->allocate_called ) return;
      const size_t write_index( Pointer::val( data->write_pt ) );
      data->store[ write_index ].signal = signal;
      
      Pointer::inc( data->write_pt );
      write_stats.all++;
      (this)->allocate_called = false;
   }

   /**
    * push- writes a single item to the queue, blocks
    * until there is enough space.
    * @param   item, T
    */
   void  push( T &item, const RBSignal signal = RBSignal::NONE )
   {
      while( space_avail() == 0 )
      {
#ifdef NICE      
         std::this_thread::yield();
#endif         
         if( write_stats.blocked == 0 )
         {   
            write_stats.blocked = 1;
         }
#if __x86_64 
         __asm__ volatile("\
           pause"
           :
           :
           : );
#endif           
      }
      
	const size_t write_index( Pointer::val( data->write_pt ) );
#if  0 //TODO, fix the byte dropping issue at the end of the write
	rb_write( (unsigned char *)&(data->store[write_index].item), 
		       (unsigned char *)&item, 
             sizeof(T), 
             feature_level );
#else      
	data->store[ write_index ].item     = item;
#endif
	data->store[ write_index ].signal   = signal;
	Pointer::inc( data->write_pt );
	write_stats.all++;
   }
   
   /**
    * insert - inserts the range from begin to end in the queue,
    * blocks until space is available.  If the range is greater than
    * available space on the queue then it'll simply add items as 
    * space becomes available.  There is the implicit assumption that
    * another thread is consuming the data, so eventually there will
    * be room.
    * @param   begin - iterator_type, iterator to begin of range
    * @param   end   - iterator_type, iterator to end of range
    */
   template< class iterator_type >
   void insert(   iterator_type begin, 
                  iterator_type end, 
                  const RBSignal signal = RBSignal::NONE )
   {
      while( begin != end )
      {
         if( space_avail() == 0 )
         {
#ifdef NICE
            std::this_thread::yield();
#endif
            if( write_stats.blocked == 0 )
            {
               write_stats.blocked = 1;
            }
         }
         else
         {
            const size_t write_index( Pointer::val( data->write_pt ) );
            data->store[ write_index ].item = (*begin);
            
            /** add signal to last el only **/
            if( begin == ( end - 1 ) )
            {
               data->store[ write_index ].signal = signal;
            }
            else
            {
               data->store[ write_index ].signal = RBSignal::NONE;
            }
            Pointer::inc( data->write_pt );
            write_stats.all++;
            begin++;
         }
      }
   }

  
   /**
    * pop - read one item from the ring buffer,
    * will block till there is data to be read
    * @return  T, item read.  It is removed from the
    *          q as soon as it is read
    */
   void pop( T &item )
   {
      while( size() == 0 )
      {
#ifdef NICE      
         std::this_thread::yield();
#endif        
         if( read_stats.blocked == 0 )
         {   
            read_stats.blocked  = 1;
         }
#if __x86_64 
         __asm__ volatile("\
           pause"
           :
           :
           : );
#endif           
      }
      const size_t read_index( Pointer::val( data->read_pt ) );
      Buffer::Element< T > &output( data->store[ read_index ] );
      /**
       * TODO, fix signalling here.  This shouldn't write over
       * previously received signals that the consumer hasn't 
       * read yet...this creates a nasty race condition if the
       * user isn't careful.
       */
      signal_mask = output.signal;
      item = output.item;
      Pointer::inc( data->read_pt );
      read_stats.all++;
      return;
   }

   /**
    * pop_range - pops a range and returns it as a std::array.  The
    * exact range to be popped is specified as a template parameter.
    * the static std::array was chosen as its a bit faster, however 
    * this might change in future implementations to a std::vector
    * or some other structure.
    */
   template< size_t N >
   void  pop_range( std::array< T, N > &output )
   {
      while( size() < N )
      {
#ifdef NICE
         std::this_thread::yield();
#endif
         if( read_stats.blocked == 0 )
         {
            read_stats.blocked = 1;
         }
      }
     
      size_t read_index;
      for( size_t i( 0 ); i < N; i++ )
      {
         read_index( Pointer::val( data->read_pt ) );
         output[ i ] = data->store[ read_index ].item;
         if( i != (N - 1 ) )
         {
            Pointer::inc( data->read_pt );
            read_stats.count++;
         }
      }
      /** TODO, perhaps might be better to consume all signals **/
      signal_mask = data->store[ read_index ].signal;
      Pointer::inc( data->read_pt );
      read_stats.count++;
      return( output );
   }


   /**
    * peek() - look at a reference to the head of the
    * ring buffer.  This doesn't remove the item, but it 
    * does give the user a chance to take a look at it without
    * removing.
    * @return T&
    */
    T& peek()
   {
      while( size() < 1 )
      {
#ifdef NICE      
         std::this_thread::yield();
#endif     
#if   __x86_64        
         __asm__ volatile("\
           pause"
           :
           :
           : );
#endif
      }
      const size_t read_index( Pointer::val( data->read_pt ) );
      T &output( data->store[ read_index ].item );
      return( output );
   }


   /**
    * recycle - To be used in conjunction with peek().  Simply
    * removes the item at the head of the queue and discards them
    * @param range - const size_t, default range is 1
    */
   void recycle( const size_t range = 1 )
   {
      assert( range <= data->max_cap );
      Pointer::incBy( range, data->read_pt );
      read_stats.count += range;
   }

protected:
   Buffer::Data< T, type>      *data;
   volatile Blocked             read_stats;
   volatile Blocked             write_stats;
   volatile RBSignal            signal_mask;
   volatile std::uint8_t        feature_level;
   volatile bool                allocate_called;
};


/**
 * Infinite / Dummy  specialization 
 */
template < class T > class RingBufferBase< T, RingBufferType::Infinite >
{
public:
   /**
    * RingBuffer - default constructor, initializes basic
    * data structures.
    */
   RingBufferBase() : data( nullptr ),
                      allocate_called( false )
   {
   }
   
   virtual ~RingBufferBase()
   {
   }


   /**
    * size - as you'd expect it returns the number of 
    * items currently in the queue.
    * @return size_t
    */
   size_t   size()
   {
      return( 1 );
   }
   
   void  send_signal( const RBSignal sig )
   {
      signal_mask = sig;
   }

   RBSignal get_signal() 
   {
      return( signal_mask );
   }

   /**
    * space_avail - returns the amount of space currently
    * available in the queue.  This is the amount a user
    * can expect to write without blocking
    * @return  size_t
    */
    size_t   space_avail()
   {
      return( data->max_cap );
   }

  
   /**
    * capacity - returns the capacity of this queue which is 
    * set at compile time by the constructor.
    * @return size_t
    */
   size_t   capacity() const
   {
      return( data->max_cap );
   }

   
   /**
    * allocate - get a reference to an object of type T at the 
    * end of the queue.  Should be released to the queue using
    * the push command once the calling thread is done with the 
    * memory.
    * @return T&, reference to memory location at head of queue
    */
   T& allocate()
   {
      (this)->allocate_called = true;
      return( data->store[ 0 ].item );
   }

   /**
    * push - releases the last item allocated by allocate() to
    * the queue.  Function will imply return if allocate wasn't
    * called prior to calling this function.
    * @param signal - const RBSignal signal, default: NONE
    */
   void push( const RBSignal signal = RBSignal::NONE )
   {
      if( ! (this)->allocate_called ) return;
      data->store[ 0 ].signal = signal;
      write_stats.all++;
      (this)->allocate_called = false;
   }

   /**
    * push - This version won't write anything, it'll
    * increment the counter and simply return;
    * @param   item, T
    */
   void  push( T &item, const RBSignal signal = RBSignal::NONE )
   {
      data->store[ 0 ].item   = item;
      /** a bit awkward since it gives the same behavior as the actual queue **/
      data->store[ 0 ].signal = signal;
      write_stats.count++;
   }

   /**
    * insert - insert a range of items into the queue.
    * @param   begin - start iterator
    * @param   end   - ending iterator
    * @param   signal - const RBSignal, set if you want to send a signal
    */
   template< class iterator_type >
   void insert( iterator_type begin, 
                iterator_type end, 
                const RBSignal signal = RBSignal::NONE )
   {
      while( begin != end )
      {
         data->store[ 0 ].item = (*begin);
         begin++;
         write_stats.count++;
      }
      data->store[ 0 ].signal = signal;
   }
 

   /**
    * pop - This version won't return any useful data,
    * its just whatever is in the buffer which should be zeros.
    * @return  T, item read.  It is removed from the
    *          q as soon as it is read
    */
   void pop( T &item )
   {
      item  = data->store[ 0 ].item;
      (this)->signal_mask = data->store[ 0 ].signal;
      read_stats.count++;
   }
  
   /**
    * pop_range - dummy function version of the real one above
    * sets the input array to whatever "dummy" data has been
    * passed into the buffer, often real data.  In most cases
    * this will allow the application to work as it would with
    * correct inputs even if the overall output will be junk.  This
    * enables correct measurement of the arrival and service rates.
    * @param output - std:;array< T, N >*
    */
   template< size_t N >
   void  pop_range( std::array< T, N > &output )
   {
      for( size_t i( 0 ); i < N; i++ )
      {
         output[ i ] = data->store[ 0 ].item;
      }
      (this)->signal_mask = data->store[ 0 ].signal;
   }


   /**
    * peek() - look at a reference to the head of the
    * the ring buffer.  This doesn't remove the item, but it 
    * does give the user a chance to take a look at it without
    * removing.
    * @return T&
    */
    T& peek()
   {
      T &output( data->store[ 0 ].item );
      return( output );
   }
   
   /**
    * recycle - remove ``range'' items from the head of the
    * queue and discard them.  Can be used in conjunction with
    * the peek operator.
    * @param   range - const size_t, default = 1
    */
   void recycle( const size_t range = 1 )
   {
      read_stats.count += range;
   }

protected:
   /** go ahead and allocate a buffer as a heap, doesn't really matter **/
   Buffer::Data< T, RingBufferType::Heap >      *data;
   volatile Blocked                             read_stats;
   volatile Blocked                             write_stats;
   volatile RBSignal                            signal_mask;
   volatile bool                                allocate_called;
};
#endif /* END _RINGBUFFERBASE_TCC_ */
