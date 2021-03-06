/*
  Remote SD card access tool for mega65_ftp to more quickly
  send and receive files.

  It implements a simple protocol with pre-emptive sending 
  of read data in raw mode at 4mbit = 40KB/sec.  Can do this
  while writing jobs to the SD card etc to hide latency.

*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <hal.h>
#include <memory.h>
#include <dirent.h>
#include <fileio.h>
#include <debug.h>
#include <random.h>

// Set to 1 to show debug messages
#define DEBUG 0

// Slight delay between chars for old HYPPO versions that lack ready check on serial write
#define SERIAL_DELAY
// #define SERIAL_DELAY for(aa=0;aa!=5;aa++) continue;
uint8_t aa;
// Write a char to the serial monitor interface
#define SERIAL_WRITE(the_char) { __asm__ ("LDA %v",the_char); __asm__ ("STA $D643"); __asm__ ("NOP"); }

uint8_t c,j,pl, job_type;
uint16_t i, job_type_addr;
void serial_write_string(uint8_t *m,uint16_t len)
{
  for(i=0;i<len;) {
    if (len-i>255) pl=255; else pl=len-i;
    i+=pl;
    for(j=0;j<pl;j++) {
      c=*m; m++;
      SERIAL_DELAY;
      SERIAL_WRITE(c);
    }    
  }
}

uint16_t last_advertise_time,current_time;
uint8_t job_count=0;
uint16_t job_addr;

char msg[80+1];

uint32_t buffer_address,sector_number,transfer_size;

uint8_t temp_sector_buffer[0x200];

uint8_t rle_count=0,a;
uint8_t olen=0,iofs=0,last_value=0;
uint8_t obuf[0x80];
uint8_t local_buffer[256];

uint8_t buffer_ready=0;
uint16_t sector_count=0;
uint8_t read_pending=0;

void rle_init(void)
{
  olen=0; iofs=0;
  last_value=0xFF; rle_count=0;
}

uint8_t block_len=0;
uint8_t bo=0;

void rle_write_string(uint32_t buffer_address,uint32_t transfer_size)
{
  lcopy(buffer_address,(uint32_t)local_buffer,256);
  
  POKE(0xD610,0);
  while (transfer_size) {
    
    // Use tight inner loop to send more quickly
    if (transfer_size&0x7f) block_len=transfer_size&0x7f;
    else block_len=0x80;

    //     printf("transfer_size=%d, block_len=%d,ba=$%x\n",transfer_size,block_len,buffer_address);
    
    transfer_size-=block_len;
    
    for(bo=0;bo<block_len;bo++) {
#if DEBUG>1
      printf("olen=%d, rle_count=%d, last=$%02x, byte=$%02x\n",
	     olen,rle_count,last_value,local_buffer[iofs]);
      while(!PEEK(0xD610)) continue; POKE(0xD610,0);
#endif
      if (olen==127)
	{
	  // Write out 0x00 -- 0x7F as length of non RLE bytes,
	  // followed by the bytes
#if DEBUG>1
	  printf("$%02x raw\n",olen);
#endif
	  SERIAL_WRITE(olen);
	  for(a=0;a<0x80;a++) {
	    c=obuf[a];
	    SERIAL_DELAY;
	    SERIAL_WRITE(c);
	  }
	  olen=0;
	  rle_count=0;
	}

      if (rle_count==127) {
	// Flush a full RLE buffer
#if DEBUG>1
	printf("$%02x x $%02x\n",rle_count,last_value);
#endif
	c=0x80|rle_count;
	SERIAL_WRITE(c);
	SERIAL_DELAY;
	SERIAL_WRITE(last_value);
#if DEBUG>1
	printf("Wrote $%02x, $%02x\n",c,last_value);
#endif
	rle_count=0;
      }
      obuf[olen++]=local_buffer[iofs];
      if (local_buffer[iofs]==last_value) {
	rle_count++;
	if (rle_count==3) {
	  // Switch from raw to RLE, flushing any pending raw bytes
	  if (olen>3) olen-=3; else olen=0;
#if DEBUG>1
	  printf("Flush $%02x raw %02x %02x %02x ...\n",olen,
		 obuf[0],obuf[1],obuf[2]);
#endif
	  if (olen) {
	    SERIAL_WRITE(olen);
	    for(a=0;a<olen;a++) {
	      c=obuf[a];
	      SERIAL_DELAY;
	      SERIAL_WRITE(c);
	    }
	  }
	  olen=0;		
	} else if (rle_count<3) {
	  // Don't do anything yet, as we haven't yet flipped to RLE coding
	} else {
	  // rle_count>3, so keep accumulating RLE data
	  olen--;
	}
      } else {
	// Flush any accumulated RLE data
	if (rle_count>2) {
#if DEBUG>1
	  printf("$%02x x $%02x\n",rle_count,last_value);
#endif
	  c=0x80|rle_count;
	  SERIAL_WRITE(c);
	  SERIAL_DELAY;
	  SERIAL_WRITE(last_value);
	}
	// 1 of the new byte seen
	rle_count=1;
      }
      
      last_value=local_buffer[iofs];
      
      // Advance and keep buffer primed
      if (iofs==0xff) {
	buffer_address+=256;
	lcopy(buffer_address,(long)&local_buffer[0],256);

	iofs=0;
      } else iofs++;
    }
  }
}

void rle_finalise(void)
{   
  // Flush any accumulated RLE data
  if (rle_count>2) {
#if DEBUG>1
    printf("Terminal $%02x x $%02x\n",rle_count,last_value);
#endif
    c=0x80|rle_count;
    SERIAL_WRITE(c);
    SERIAL_DELAY;
    SERIAL_WRITE(last_value);
  } else if (olen) {
#if DEBUG>1
    printf("Terminal flush $%02x raw %02x %02x %02x ...\n",olen,
	   obuf[0],obuf[1],obuf[2]);
#endif
    SERIAL_WRITE(olen);
    for(a=0;a<olen;a++) {
      c=obuf[a];
      SERIAL_DELAY;
      SERIAL_WRITE(c);
    }
  }
}


void main(void)
{
  unsigned char jid;
  
  asm ( "sei" );

  // Fast CPU, M65 IO
  POKE(0,65);
  POKE(0xD02F,0x47);
  POKE(0xD02F,0x53);

  // Cursor off
  POKE(204,0x80);
  
  printf("%cMEGA65 File Transfer helper.\n",0x93);

  last_advertise_time=0;

  // Clear communications area
  lfill(0xc000,0x00,0x1000);
  lfill(0xc001,0x42,0x1000);
  
  while(1) {
    current_time=*(uint16_t *)0xDC08;
    if (current_time!=last_advertise_time) {
      // Announce our protocol version
      serial_write_string("\nmega65ft1.0\n\r",14);
      last_advertise_time=current_time;
    }

    if (PEEK(0xC000)) {
      // Perform one or more jobs
      job_count=PEEK(0xC000);
#if DEBUG
      printf("Received list of %d jobs.\n",job_count);
#endif
      job_addr=0xc001;
      for(jid=0;jid<job_count;jid++) {
	if (job_addr>0xcfff) break;
	job_type_addr = job_addr;
	job_type = PEEK(job_type_addr);
	switch(job_type) {
	case 0x00: job_addr++; break;
	case 0xFF:
	  // Terminate
	  __asm__("jmp 58552");
	  break;
	case 0x02:
	case 0x05: // multi write first
	case 0x06: // multi write middle
	case 0x07: // multi write end
	  // Write sector
	  job_addr++;
	  buffer_address=*(uint32_t *)job_addr;
	  job_addr+=4;
	  sector_number=*(uint32_t *)job_addr;
	  job_addr+=4;

#if DEBUG>0
	  printf("$%04x : write sector $%08lx from mem $%07lx\n",*(uint16_t *)0xDC08,
		 sector_number,buffer_address);
#endif

	  //	  lcopy(buffer_address,0x0400,512);
	  lcopy(buffer_address,0xffd6e00,512);
	  
	  // Write sector
	  *(uint32_t *)0xD681 = sector_number;	  
	  POKE(0xD680,0x57); // Open write gate
	  if (job_type==0x02) POKE(0xD680,0x03);
	  if (job_type==0x05) POKE(0xD680,0x04);
	  if (job_type==0x06) POKE(0xD680,0x05);
	  if (job_type==0x07) POKE(0xD680,0x06);

	  // Wait for SD to go busy
	  while(!(PEEK(0xD680)&0x03)) continue;
	  
	  // Wait for SD to read and fiddle border colour to show we are alive
	  while(PEEK(0xD680)&0x03) POKE(0xD020,PEEK(0xD020)+1);
	  
	  break;
	case 0x01:
	  // Read sector
	  job_addr++;
	  buffer_address=*(uint32_t *)job_addr;
	  job_addr+=4;
	  sector_number=*(uint32_t *)job_addr;
	  job_addr+=4;

#if DEBUG
	  printf("$%04x : Read sector $%08lx into mem $%07lx\n",*(uint16_t *)0xDC08,
		 sector_number,buffer_address);
#endif
	  // Do read
	  *(uint32_t *)0xD681 = sector_number;	  
	  POKE(0xD680,0x02);

	  // Wait for SD to go busy
	  while(!(PEEK(0xD680)&0x03)) continue;
	  
	  // Wait for SD to read and fiddle border colour to show we are alive
	  while(PEEK(0xD680)&0x03) POKE(0xD020,PEEK(0xD020)+1);

	  lcopy(0xffd6e00,buffer_address,0x200);

	  snprintf(msg,80,"ftjobdone:%04x:\n\r",job_type_addr);
	  serial_write_string(msg,strlen(msg));

#if DEBUG
	  printf("$%04x : Read sector done\n",*(uint16_t *)0xDC08);
#endif
	  
	  break;
	case 0x03: // 0x03 == with RLE
	case 0x04: // 0x04 == no RLE
	  // Read sectors and stream
	  job_addr++;
	  sector_count=*(uint16_t *)job_addr;
	  job_addr+=2;
	  sector_number=*(uint32_t *)job_addr;
	  job_addr+=4;

	  // Begin with no bytes to send
	  buffer_ready=0;
	  // and no sector in progress being read
	  read_pending=0;

	  // Reset RLE state
	  rle_init();

	  if (job_type==3) snprintf(msg,80,"ftjobdata:%04x:%08lx:",job_type_addr,sector_count*0x200L);
	  else snprintf(msg,80,"ftjobdatr:%04x:%08lx:",job_type_addr,sector_count*0x200L);
	  serial_write_string(msg,strlen(msg));	    

	  while(sector_count||buffer_ready||read_pending)
	    {
	      if (sector_count&&(!read_pending))
		if (!(PEEK(0xD680)&0x03)) {
		  // Schedule reading of next sector

		  POKE(0xD020,PEEK(0xD020)+1);
 
		  // Do read
		  *(uint32_t *)0xD681 = sector_number;

		  read_pending=1;
		  sector_count--;

		  POKE(0xD680,0x02);
		  // Wait for SD card to go busy
		  while (!(PEEK(0xD680)&0x03)) {
		    continue;
		  }
		  
		  sector_number++;

		}
	      if (read_pending&&(!buffer_ready)) {
		
		// Read is complete, now queue it for sending back
		if (!(PEEK(0xD680)&0x03)) {
		  // Sector has been read. Copy it to a local buffer for sending,
		  // so that we can send it while reading the next sector
		  lcopy(0xffd6e00,(long)&temp_sector_buffer[0],0x200);
		  read_pending=0;
		  buffer_ready=1;
		}
	      }
	      if (buffer_ready) {
		
		// XXX - Just send it all in one go, since we don't buffer multiple
		// sectors
		if (job_type==3)
		  rle_write_string(temp_sector_buffer,0x200);
		else
		  serial_write_string(temp_sector_buffer,0x200);
		buffer_ready = 0;		
	      }
	    }

	  if (job_type==3)
	    rle_finalise();
	  
#if DEBUG
	  printf("$%04x : Read sector $%08lx into mem $%07lx\n",*(uint16_t *)0xDC08,
		 sector_number,buffer_address);
#endif

	  snprintf(msg,80,"ftjobdone:%04x:\n\r",job_type_addr);
	  serial_write_string(msg,strlen(msg));

#if DEBUG
	  printf("$%04x : Read sector done\n",*(uint16_t *)0xDC08);
#endif
	  
	  break;

	case 0x11:
	  // Send block of memory
	  job_addr++;
	  buffer_address=*(uint32_t *)job_addr;
	  job_addr+=4;
	  transfer_size=*(uint32_t *)job_addr;
	  job_addr+=4;

#if DEBUG
	  printf("$%04x : Send mem $%07lx to $%07lx: %02x %02x ...\n",*(uint16_t *)0xDC08,
		 buffer_address,buffer_address+transfer_size-1,
		 lpeek(buffer_address),lpeek(buffer_address+1));
#endif	  

	  snprintf(msg,80,"ftjobdata:%04x:%08lx:",job_type_addr,transfer_size);
	  serial_write_string(msg,strlen(msg));	    
	  
	  // Set up buffers
	  rle_init();
	  rle_write_string(buffer_address,transfer_size);
	  rle_finalise();
	 
	  snprintf(msg,80,"ftjobdone:%04x:\n\r",job_type_addr);
	  serial_write_string(msg,strlen(msg));

#if DEBUG
	  printf("$%04x : Send mem done\n",*(uint16_t *)0xDC08);
#endif
	  
	  break;
	  
	default:
	  job_addr=0xd000;
	  break;
	}
      }

      // Indicate when we think we are all done      
      POKE(0xC000,0);      
      snprintf(msg,80,"ftbatchdone\n");
      serial_write_string(msg,strlen(msg));

#if DEBUG
      printf("$%04x : Sending batch done\n",*(uint16_t *)0xDC08);
#endif      
    }
    
  }
  
}
