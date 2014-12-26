#include <string.h>
#include <math.h>
#include "stm32f10x.h"
#include "main.h"
#include "adc.h"
#include "gpio.h"
#include "usart.h"
#include "interrupts.h"
#include "pwm.h"
#include "watchdog.h"
#include "Util/rprintf.h"
#include "Util/delay.h"
#include "usb_lib.h"
#include "Util/USB/hw_config.h"
#include "Util/USB/usb_pwr.h"
#include "Util/fat_fs/inc/diskio.h"
#include "Util/fat_fs/inc/ff.h"
#include "Util/fat_fs/inc/integer.h"
#include "Util/fat_fs/inc/rtc.h"

//newlib reent context
struct _reent my_reent;
     
//Global variables - other files (e.g. hardware interface/drivers) may have their own globals
extern uint16_t MAL_Init (uint8_t lun);			//For the USB filesystem driver
volatile uint8_t file_opened=0;				//So we know to close any opened files before turning off
uint8_t print_string[256];				//For printf data
UINT a;							//File bytes counter

//FatFs filesystem globals go here
FRESULT f_err_code;
static FATFS FATFS_Obj;
FIL FATFS_logfile;
FILINFO FATFS_info;
//volatile int bar[3] __attribute__ ((section (".noinit"))) ;//= 0xaa

int main(void)
{
	uint8_t system_state=0;				//used to track button press functionality
	uint8_t sensors=0;
	int8_t RTC_Offset;				//RTC correction from setting file
	float sensor_data;
	uint8_t UplinkFlags=0,CutFlags=0;
	uint16_t UplinkBytes=0;				//Counters and flags for telemetry
	RTC_t RTC_time;
        _REENT_INIT_PTR(&my_reent);
        _impure_ptr = &my_reent;
	SystemInit();					//Sets up the clk
	setup_gpio();					//Initialised pins, and detects boot source
	DBGMCU_Config(DBGMCU_IWDG_STOP, ENABLE);	//Watchdog stopped during JTAG halt
	if(RCC->CSR&RCC_CSR_IWDGRSTF) {			//Watchdog reset, turn off
		RCC->CSR|=RCC_CSR_RMVF;			//Reset the reset flags
		shutdown();
	}
	SysTick_Configuration();			//Start up system timer at 100Hz for uSD card functionality
	Watchdog_Config(WATCHDOG_TIMEOUT);		//Set the watchdog
	Watchdog_Reset();				//Reset watchdog as soon as possible incase it is still running at power on
	rtc_init();					//Real time clock initialise - (keeps time unchanged if set)
	Usarts_Init();
	ISR_Config();
	rprintfInit(__usart_send_char);			//Printf over the bluetooth
	if(USB_SOURCE==bootsource) {
		Set_System();				//This actually just inits the storage layer
		Set_USBClock();
		USB_Interrupts_Config();
		USB_Init();
		uint32_t nojack=0x000FFFFF;		//Countdown timer - a few hundered ms of 0v on jack detect forces a shutdown
		while (bDeviceState != CONFIGURED) {	//Wait for USB config - timeout causes shutdown
			if((Millis>10000 && bDeviceState == UNCONNECTED)|| !nojack)	//No USB cable - shutdown (Charger pin will be set to open drain, cant be disabled without usb)
				shutdown();
			if(GET_VBUS_STATE)		//Jack detect resets the countdown
				nojack=0x0FFFFF;
			nojack--;
			Watchdog_Reset();		//Reset watchdog here, if we are stalled here the Millis timeout should catch us
		}
	}
	if(!GET_PWR_STATE)				//Check here to make sure the power button is still pressed, if not, sleep
		shutdown();				//This means a glitch on the supply line, or a power glitch results in sleep
	// check to see if battery has enough charge to start
	ADC_Configuration();				//We leave this a bit later to allow stabilisation
	Delay(100000);					//Sensor+inst amplifier takes about 100ms to stabilise after power on
	if(Battery_Voltage<BATTERY_STARTUP_LIMIT)	//We will have to turn off
		shutdown();
	// system has passed battery level check and so file can be opened
	if((f_err_code = f_mount(0, &FATFS_Obj)))Usart_Send_Str((char*)"FatFs mount error\r\n");//This should only error if internal error
	else {						//FATFS initialised ok, try init the card, this also sets up the SPI1
		if(!f_open(&FATFS_logfile,"time.txt",FA_OPEN_EXISTING | FA_READ | FA_WRITE)) {//Try and open a time file to get the system time
			if(!f_stat((const TCHAR *)"time.txt",&FATFS_info)) {//Get file info
				if(FATFS_info.fsize<5) {	//Empty file
					RTC_time.year=(FATFS_info.fdate>>9)+1980;//populate the time struct (FAT start==1980, RTC.year==0)
					RTC_time.month=(FATFS_info.fdate>>5)&0x000F;
					RTC_time.mday=FATFS_info.fdate&0x001F;
					RTC_time.hour=(FATFS_info.ftime>>11)&0x001F;
					RTC_time.min=(FATFS_info.ftime>>5)&0x003F;
					RTC_time.sec=(FATFS_info.ftime<<1)&0x003E;
					rtc_settime(&RTC_time);
					rprintfInit(__fat_print_char);//printf to the open file
					printf("RTC set to %d/%d/%d %d:%d:%d\n",RTC_time.mday,RTC_time.month,RTC_time.year,\
					RTC_time.hour,RTC_time.min,RTC_time.sec);
				}				
			}
			f_close(&FATFS_logfile);	//Close the time.txt file
		}
		// load settings if file exists
		if(!f_open(&FATFS_logfile,"settings.dat",FA_OPEN_EXISTING | FA_READ)) {
		  UINT br;
		  f_read(&FATFS_logfile, (void*)(&RTC_Offset),sizeof(RTC_Offset),&br);
		  f_close(&FATFS_logfile);	//Close the settings.dat file
		}
#ifndef SINGLE_LOGFILE
		rtc_gettime(&RTC_time);			//Get the RTC time and put a timestamp on the start of the file
		rprintfInit(__str_print_char);		//Print to the string
		printf("%02d-%02d-%02dT%02d-%02d-%02d-%s.csv",RTC_time.year,RTC_time.month,RTC_time.mday,RTC_time.hour,RTC_time.min,RTC_time.sec,"Log");//Timestamp name
		rprintfInit(__usart_send_char);		//Printf over the bluetooth
#endif
		if((f_err_code=f_open(&FATFS_logfile,LOGFILE_NAME,FA_CREATE_ALWAYS | FA_WRITE))) {//Present
			printf("FatFs drive error %d\r\n",f_err_code);
			if(f_err_code==FR_DISK_ERR || f_err_code==FR_NOT_READY)
				Usart_Send_Str((char*)"No uSD card inserted?\r\n");
		}
		else {					//We have a mounted card
			f_err_code=f_lseek(&FATFS_logfile, PRE_SIZE);// Pre-allocate clusters
			if (f_err_code || f_tell(&FATFS_logfile) != PRE_SIZE)// Check if the file size has been increased correctly
				Usart_Send_Str((char*)"Pre-Allocation error\r\n");
			else {
				if((f_err_code=f_lseek(&FATFS_logfile, 0)))//Seek back to start of file to start writing
					Usart_Send_Str((char*)"Seek error\r\n");
				else
					rprintfInit(__str_print_char);//Printf to the logfile
			}
			if(f_err_code)
				f_close(&FATFS_logfile);//Close the already opened file on error
			else
				file_opened=1;		//So we know to close the file properly on shutdown
		}
	}
	if(f_err_code) {				//There was an init error
		shutdown();				//Abort after a single red flash ------------------ABORT 1
	}
	Watchdog_Reset();				//Card Init can take a second or two
	I2C_Config();					//Setup the I2C bus
	sensors=detect_sensors();
	if(sensors&((1<<L3GD20_CONFIG)|(1<<AFROESC_READ))!=((1<<L3GD20_CONFIG)|(1<<AFROESC_READ))) {
		f_puts("I2C sensor detect error",&FATFS_logfile);
		f_close(&FATFS_logfile);		//So we log that something went wrong in the logfile
		shutdown();
	}
	rtc_gettime(&RTC_time);				//Get the RTC time and put a timestamp on the start of the file
	print_string[0]=0x00;				//Set string length to 0
	printf("%02d-%02d-%02dT%02d:%02d:%02d\n",RTC_time.year,RTC_time.month,RTC_time.mday,RTC_time.hour,RTC_time.min,RTC_time.sec);//ISO 8601 timestamp header
        sensor_data=Battery_Voltage;			//Have to flush adc for some reason
        Delay(10000);
	printf("Battery: %3fV\n",Battery_Voltage);	//Get the battery voltage using blocking regular conversion and print
	printf("Time");					//Print out a header for columns that are present in the CSV file
	printf("Lat,Long,Alt,Voltage,Aux_Voltage,XY_Gyro,Z_Gyro,Temperature,Uplink(Bytes),Uplink_CommandFlags,Cutdown,Spin,Ind,Button press\r\n");
	if(file_opened) {
		f_puts(print_string,&FATFS_logfile);
		print_string[0]=0x00;			//Set string length to 0
	}
	Millis=0;					//Reset system uptime, we have 50 days before overflow
	while (1) {					//Main loop
		Watchdog_Reset();			//Reset the watchdog each main loop iteration
		while(1)
			__WFI();			//Wait for some PPG data
		
		//Other sensors etc can go here
		//Button multipress status
		if(System_state_Global&0x80) {		//A "control" button press
			system_state=System_state_Global&~0x80;//Copy to local variable
			if(system_state==1)		//A single button press
				PPG_Automatic_Brightness_Control();//At the moment this is the only function implimented
			System_state_Global&=~0x80;	//Wipe the flag bit to show this has been processed
		}
		 printf(",%d\n",system_state);		//Terminating newline
		//Can do other things with the system state here
		system_state=0;				//Reset this
		if(file_opened  & 0x01) {
			f_puts(print_string,&FATFS_logfile);
			print_string[0]=0x00;		//Set string length to 0
		}
		//Deal with file size - may need to preallocate some more
		if(f_size(&FATFS_logfile)-f_tell(&FATFS_logfile)<(PRE_SIZE/2)) {//More than half way through the pre-allocated area
			DWORD size=f_tell(&FATFS_logfile);
			f_lseek(&FATFS_logfile, f_size(&FATFS_logfile)+PRE_SIZE);//preallocate another PRE_SIZE
			f_lseek(&FATFS_logfile, size);	//Seek back to where we were before
		}
                if(Shutdown_System) {			//A system shutdown has been requested
			if(file_opened)
				shutdown_filesystem(Shutdown_System, file_opened);
			if(Shutdown_System==USB_INSERTED)
				NVIC_SystemReset();	//Software reset of the system - USB inserted whilst running
			else {
				shutdown();		//Puts us into sleep mode
			}
		}
	}
}

/**
  * @brief  Writes a char to logfile
  * @param  Character to write
  * @retval None
  */
void __fat_print_char(char c) {
	f_write(&FATFS_logfile,&c,(UINT)1,&a);
}

/**
  * @brief  Writes a char to string - use for better logfile performance
  * @param  Character to write
  * @retval None
  */
void __str_print_char(char c) {
	uint8_t indx=strlen(print_string)%255;		//Make sure we cant overwrite ram
	print_string[indx]=c;				//Append string
	print_string[indx+1]=0x00;			//Null terminate
	__usart_send_char(c);				//Send to the bluetooth as well
}


/**
  * @brief  Detects which sensors are plugged in, inits buffers for attached peripheral sensors
  * @param  None
  * @retval Bitmask of detected sensors
  */
uint8_t detect_sensors(void) {
        uint32_t millis = Millis;
	uint8_t sensors=0;
	SCHEDULE_CONFIG;				//Run the I2C devices config
	while(Jobs) {//while((I2C1->CR2)&(I2C_IT_EVT));//Wait for th i2c driver to complete
	  if(Millis>(millis+20))
	    return 0;
	}
	sensors=Completed_Jobs;				//Which I2C jobs completed ok?
	return sensors;
}