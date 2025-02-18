/*
 * Title:			Agon MOS firmware upgrade utility
 * Author:			Jeroen Venema
 * Created:			17/12/2022
 * Last Updated:	17/12/2022
 * 
 * Modinfo:
 * 17/12/2022:		Initial version
 * 05/04/2022:		Changed timer to 5sec at reset.
 *                  Sends cls just before reset
 * 07/06/2023:		Included faster crc32, by Leigh Brown
 */

#include <ez80.h>
#include <stdio.h>
#include <stdlib.h>
#include <ERRNO.H>
#include "mos-interface.h"
#include "flash.h"
#include "agontimer.h"
#include "crc32.h"

int errno; // needed by standard library
enum states{firmware,recover,systemreset};

// separate putch function that doesn't rely on a running MOS firmware
// UART0 initialization done by MOS firmware previously
// This utility doesn't run without MOS to load it anyway
int putch(int c)
{
	UINT8 lsr,temt;
	
	while((UART0_LSR & 0x40) == 0);
	UART0_THR = c;
	return c;
}

int main(int argc, char * argv[]) {
	UINT32 crcexpected,crcresult,crcbackup;
	UINT24 size = 0;
	UINT24 got;
	UINT8 file;
	char* ptr = (char*)BUFFER1;
	UINT8 response;
	UINT8 value;
	UINT24 counter,pagemax, lastpagebytes;
	UINT24 addressto,addressfrom;
	enum states state;
	
	printf("Agon MOS firmware upgrade utility v1.3\n\r\n\r");
	
	if(argc != 3)
	{
		printf("Usage: FLASH <filename> <crc32>\n\r\r\n");
		printf("       <filename> - e.g. firmware.bin\r\n");
		printf("       <crc32> - 4byte HEX CRC32 checksum\r\n");
		return 0;
	}

	file = mos_fopen(argv[1], fa_read);
	if(!file)
	{
		printf("Error opening \"%s\"\n\r",argv[1]);
		return 0;
	}
	
	crcexpected = strtoll(argv[2]);
	if(errno)
	{
		printf("Incorrect crc32 format\n\r");
		return 0;
	}

	printf("Loading file : %s\n\r",argv[1]);
	printf("File size    : %d byte(s)", size);

	// Read file to memory
	while((got = mos_fread(file, ptr, BLOCKSIZE)) > 0)
	{
		ptr += got;
		size += got;
		printf("\rFile size    : %d byte(s)", size);
	}		
	mos_fclose(file);	
	printf("\rFile size    : %d byte(s)\n\r", size);
	if(size > FLASHSIZE)
	{
		printf("Too large for 128KB flash - aborting\r\n");
		return 0;
	}

	printf("Testing CRC32: 0x%08lx\n\r",crcexpected);
	crcresult = crc32((char*)BUFFER1, size);
	printf("CRC32 result : 0x%08lx\n\r",crcresult);

	if(crcexpected != crcresult)
	{
		printf("\n\rMismatch - aborting\n\r");
		return 0;
	}
	printf("\n\rOK\n\r\n\r");

	// Ask user to continue
	printf("Erase and program flash (y/n)? ");
	response = 0;
	while((response != 'y') && (response != 'n')) response = getch();
	if(response == 'y')
	{
		printf("\r\nBacking up existing firmware... ");
		fastmemcpy(BUFFER2, 0x0, FLASHSIZE);	
		crcbackup = crc32((char*)0x0, FLASHSIZE);
		
		di();								// prohibit any access to the old MOS firmware

		// start address in flash
		addressto = FLASHSTART;
		addressfrom = BUFFER1;
		
		state = firmware;		
		while(1)
		{
			switch(state)
			{
				case firmware:
					// start address in flash
					addressfrom = BUFFER1;					
					break;
				case recover:
					// start address in flash
					addressfrom = BUFFER2;
					size = FLASHSIZE;			// entire backup buffer
					break;
				default:
					// RESET SYSTEM
					printf("\r\n");
					for(counter = 5; counter >0; counter--)
					{
						printf("\rReset in %ds",counter);
						delayms(1000);
					}
					putch(12);
					delayms(500);
					reset();
			}
	
			// Unprotect and erase flash
			printf("\r\nErasing flash... ");
			enableFlashKeyRegister();	// unlock Flash Key Register, so we can write to the Flash Write/Erase protection registers
			FLASH_PROT = 0;				// disable protection on all 8x16KB blocks in the flash
			enableFlashKeyRegister();	// will need to unlock again after previous write to the flash protection register
			FLASH_FDIV = 0x5F;			// Ceiling(18Mhz * 5,1us) = 95, or 0x5F
			
			for(counter = 0; counter < FLASHPAGES; counter++)
			{
				FLASH_PAGE = counter;
				FLASH_PGCTL = 0x02;			// Page erase bit enable, start erase

				do
				{
					value = FLASH_PGCTL;
				}
				while(value & 0x02);// wait for completion of erase			
			}
			
			printf("\r\nWriting new firmware...\r\n");
			
			// determine number of pages to write
			pagemax = size/PAGESIZE;
			if(size%PAGESIZE) // last page has less than PAGESIZE bytes
			{
				pagemax += 1;
				lastpagebytes = size%PAGESIZE;			
			}
			else lastpagebytes = PAGESIZE; // normal last page
			
			// write out each page to flash
			for(counter = 0; counter < pagemax; counter++)
			{
				printf("\rWriting flash page %03d/%03d", counter+1, pagemax);
				
				if(counter == (pagemax - 1)) // last page to write - might need to write less than PAGESIZE
					fastmemcpy(addressto,addressfrom,lastpagebytes);				
					//printf("Fake copy to %lx, from %lx, %lx bytes\r\n",addressto, addressfrom, lastpagebytes);
				else 
					fastmemcpy(addressto,addressfrom,PAGESIZE);
					//printf("Fake copy to %lx, from %lx, %lx bytes\r\n",addressto, addressfrom, PAGESIZE);
			
				addressto += PAGESIZE;
				addressfrom += PAGESIZE;
			}
			lockFlashKeyRegister();	// lock the flash before WARM reset
			printf("\r\n");
			
			//Verify correct CRC in flash
			printf("Verifying flash checksum... ");
			crcresult = crc32((char*)FLASHSTART, size);

			if(state == recover) crcexpected = crcbackup;
			
			if(crcresult == crcexpected)
			{
				printf("- OK\r\n");
				state = systemreset;
			}
			else // CRC Failure - next action depends on current state
			{	 // User interaction not possible without MOS handling interrupts
				switch(state)
				{
					case firmware:
						printf("\r\nError occured during flash write\r\nAttempting to flash backup firmware...\r\n");
						state = recover;
						break;
					case recover:
						printf("\r\nError occured during flash write\r\nBackup recovery failed\r\n");
						while(1); // no more options unfortunately, system needs a firmware programmer
					default:
						state = recover;
				}
			}
		}		
	}
	else printf("\n\rUser abort\n\r");
	
	return 0;
}

