//Dactyl project v1.0
//I2C interrupt code using the ST Perif library
#include "i2c_int.h"
#include "gpio.h"
#include "Util/delay.h"

//Globals for the data IO to the driver
volatile uint8_t L3GD20_Data_Buffer[8];
volatile uint8_t AFROESC_Data_Buffer[7];
volatile uint16_t AFROESC_Throttle;	//note that this is big endian 

volatile buff_type Gyro_x_buffer,Gyro_y_buffer,Gyro_z_buffer,Gyro_aligned_rpm_buffer;//Buffers for raw data

//Globals for the driver
volatile uint32_t Jobs,Completed_Jobs;	//used for task control (only ever access this from outside for polling Jobs/Reading Completed_Jobs)
volatile uint8_t job;			//stores the current job
volatile I2C_Error_Type I2C1error;	//stores current error status

//Setup the const jobs descriptors
volatile I2C_Job_Type I2C_jobs[]=I2C_JOBS_INITIALISER;//sets up the const jobs

/**
  * @brief  This function handles I2C1 Event interrupt request.
  * @param : None
  * @retval : None
  */
void I2C1_EV_IRQHandler(void) {
	static uint8_t subaddress_sent,final_stop;//flag to indicate if subaddess sent, flag to indicate final bus condition
	static int8_t index;		//index is signed -1==send the subaddress
	volatile uint32_t debugme;
	volatile uint8_t SReg_1=I2C1->SR1;//read the status register here
	if(!((Jobs>>job)&0x00000001)) {	//if the current job bit is not set
		for(job=0;!((Jobs>>job)&0x00000001) && job<(I2C_NUMBER_JOBS-1);job++);//find the first uncompleted job, starting at current job zero
		subaddress_sent=0;
		if(!Jobs)
			I2C_GenerateSTOP(I2C1,ENABLE);//program a stop to get out of here
	}
	if(SReg_1&0x0001 && !((SReg_1&0x0040) && (I2C1->CR2&0x400) && subaddress_sent)) {//we just sent a start -EV5 in ref manual, but no RXNE + BUF event
		I2C_AcknowledgeConfig(I2C1, ENABLE);//make sure ACK is on
		index=0;		//reset the index
		if(I2C_Direction_Receiver==I2C_jobs[job].direction && (subaddress_sent || 0xFF==I2C_jobs[job].subaddress)) {//we have sent the subaddr
			subaddress_sent=1;//make sure this is set in case of no subaddress, so following code runs correctly
			if(2==I2C_jobs[job].bytes)
				I2C1->CR1|=0x0800;//set the POS bit so NACK applied to the final byte in the two byte read
			I2C_Send7bitAddress(I2C1,I2C_jobs[job].address,I2C_Direction_Receiver);//send the address and set hardware mode
		}
		else {			//direction is Tx, or we havent sent the sub and rep start
			I2C_Send7bitAddress(I2C1,I2C_jobs[job].address,I2C_Direction_Transmitter);//send the address and set hardware mode
			if(0xFF!=I2C_jobs[job].subaddress)//0xFF as subaddress means it will be ignored, in Tx or Rx mode
				index=-1;//send a subaddress
		}
	}
	else if(SReg_1&0x0002) {//we just sent the address - EV6 in ref manual
		//Read SR1,2 to clear ADDR
		volatile uint8_t a;
		asm volatile ("dmb" ::: "memory");//memory fence to control hardware
		if(1==I2C_jobs[job].bytes && I2C_Direction_Receiver==I2C_jobs[job].direction && subaddress_sent) {//we are receiving 1 byte - EV6_3
			I2C_AcknowledgeConfig(I2C1, DISABLE);//turn off ACK
			asm volatile ("dmb" ::: "memory");
			a=I2C1->SR2;	//clear ADDR after ACK is turned off
			I2C_GenerateSTOP(I2C1,ENABLE);//program the stop
			final_stop=1;
			I2C_ITConfig(I2C1, I2C_IT_BUF, ENABLE);//allow us to have an EV7
		}
		else {//EV6 and EV6_1
			a=I2C1->SR2;	//clear the ADDR here
			asm volatile ("dmb" ::: "memory");
			if(2==I2C_jobs[job].bytes && I2C_Direction_Receiver==I2C_jobs[job].direction && subaddress_sent) { //rx 2 bytes - EV6_1
				I2C_AcknowledgeConfig(I2C1, DISABLE);//turn off ACK
				I2C_ITConfig(I2C1, I2C_IT_BUF, DISABLE);//disable TXE to allow the buffer to fill
			}
			else if(3==I2C_jobs[job].bytes && I2C_Direction_Receiver==I2C_jobs[job].direction && subaddress_sent)//rx 3 bytes
				I2C_ITConfig(I2C1, I2C_IT_BUF, DISABLE);//make sure RXNE disabled so we get a BTF in two bytes time
			else //receiving greater than three bytes, sending subaddress, or transmitting
				I2C_ITConfig(I2C1, I2C_IT_BUF, ENABLE);
		}
	}
	else if((SReg_1&0x004) && !(I2C1->CR2&0x400)) {//Byte transfer finished - EV7_2, EV7_3 or EV8_2 (Check that BUF disabled)
		if(Jobs&~(1<<job)) 	//check if there are other jobs requested other than the current one
			final_stop=0;
		else
			final_stop=1;
		if(I2C_Direction_Receiver==I2C_jobs[job].direction && subaddress_sent) {//EV7_2, EV7_3
			if(I2C_jobs[job].bytes>2) {//EV7_2 
				I2C_AcknowledgeConfig(I2C1, DISABLE);//turn off ACK
				volatile uint8_t dummy;
				if(I2C_jobs[job].data_pointer)
					I2C_jobs[job].data_pointer[index++]=I2C_ReceiveData(I2C1);//read data N-2
				else {
					dummy=I2C_ReceiveData(I2C1);
					index++;
				}
				asm volatile ("dmb" ::: "memory");
				if(final_stop)
					I2C_GenerateSTOP(I2C1,ENABLE);//program the Stop
				else
					I2C_GenerateSTART(I2C1,ENABLE);//program a rep start
				if(I2C_jobs[job].data_pointer)
					I2C_jobs[job].data_pointer[index++]=I2C_ReceiveData(I2C1);//read data N-1
				else {
					dummy=I2C_ReceiveData(I2C1);
					index++;
				}
				I2C_ITConfig(I2C1, I2C_IT_BUF, ENABLE);//enable TXE to allow the final EV7
			}
			else {		//EV7_3
				if(final_stop)
					I2C_GenerateSTOP(I2C1,ENABLE);//program the Stop
				else
					I2C_GenerateSTART(I2C1,ENABLE);//program a rep start
				asm volatile ("dmb" ::: "memory");
				if(I2C_jobs[job].data_pointer) {
					I2C_jobs[job].data_pointer[index++]=I2C_ReceiveData(I2C1);//read data N-1
					I2C_jobs[job].data_pointer[index++]=I2C_ReceiveData(I2C1);//read data N
				}
				else {
					volatile uint8_t dummy=I2C_ReceiveData(I2C1);
					dummy=I2C_ReceiveData(I2C1);
					index+=2;
				}
				index++;//to show job completed
			}
		}
		else {//EV8_2, which may be due to a subaddress sent or a write completion
			volatile uint8_t dummy=I2C_ReceiveData(I2C1);//dummy read to clear BTF
			if(subaddress_sent || (I2C_Direction_Transmitter==I2C_jobs[job].direction)) {
				if(final_stop)
					I2C_GenerateSTOP(I2C1,ENABLE);//program the Stop
				else
					I2C_GenerateSTART(I2C1,ENABLE);//program a rep start
				index++;//to show that the job is complete
			}
			else {		//We need to send a subaddress
				I2C_GenerateSTART(I2C1,ENABLE);//program the repeated Start
				subaddress_sent=1;//this is set back to zero upon completion of the current task
			}
		}
	}
	else if(SReg_1&0x0040 && (I2C1->CR2&0x400)) {//Byte received - EV7 
		//(note that at end of >2 byte read with rep start, we may get RXNE+START in same call, so this will not clear the start bit)
		volatile uint8_t dummy=I2C_ReceiveData(I2C1);
		asm volatile ("dmb" ::: "memory");
		if(I2C_jobs[job].data_pointer)
			I2C_jobs[job].data_pointer[index++]=dummy;
		else
			index++;	
		if(I2C_jobs[job].bytes==(index+3))
			I2C_ITConfig(I2C1, I2C_IT_BUF, DISABLE);//disable TXE to allow the buffer to flush so we can get an EV7_2
		else if(I2C_jobs[job].bytes==index)//We have completed a final EV7
			index++;	//to show job is complete
	}
	else if(SReg_1&0x0080) {//Byte transmitted -EV8/EV8_1
		if(-1!=index && (I2C1->CR2&0x400)) {	//we dont have a subaddress to send, and BUF is enabled
			I2C_SendData(I2C1,I2C_jobs[job].data_pointer[index++]);
			if(I2C_jobs[job].bytes==index)//we have sent all the data
				I2C_ITConfig(I2C1, I2C_IT_BUF, DISABLE);//disable TXE to allow the buffer to flush
		}		
		else if(I2C1->CR2&0x400) {
			index++;
			if(I2C_Direction_Receiver==I2C_jobs[job].direction || !I2C_jobs[job].bytes)//if receiving or sending 0 bytes, flush now
				I2C_ITConfig(I2C1, I2C_IT_BUF, DISABLE);//disable TXE to allow the buffer to flush
			I2C_SendData(I2C1,I2C_jobs[job].subaddress);//send the subaddress
		}
	}
	if((I2C_jobs[job].bytes+1)==index) {//we have completed the current job
		//Completion Tasks go here
		if(job==L3GD20_STATUS) {
			if((L3GD20_Data_Buffer[1]&0x0F)==0x0F)
				Jobs|=(1<<L3GD20_READ);//If there is data we read the data from the device
		}
		//End of completion tasks
		Jobs&=~(0x00000001<<job);//tick off current job as complete
		Completed_Jobs|=(0x00000001<<job);//These can be polled by other tasks to see if a job has been completed or is scheduled 
		subaddress_sent=0;	//reset these here
		job=0;
		I2C1->CR1&=~0x0800;	//reset the POS bit so NACK applied to the current byte
		I2C_ITConfig(I2C1, I2C_IT_BUF, DISABLE);//make sure the TXNE/RXE is disabled to start off with
		if(Jobs && final_stop) {//there are still jobs left
			uint16_t timeout=50000;
			while(I2C1->CR1&0x0200 && timeout){timeout++;}//doesnt seem to be a better way to do this, must wait for stop to clear
			if(!timeout) {//timeout error - save the job
				I2C1error.error=0x30;
				I2C1error.job=job;
				return;
			}
			I2C_GenerateSTART(I2C1,ENABLE);//program the Start to kick start the new transfer
		}
		else if(final_stop)	//If there is a final stop and no more jobs, bus is inactive, disable interrupts to prevent BTF
			I2C_ITConfig(I2C1, I2C_IT_EVT|I2C_IT_ERR, DISABLE);//Disable EVT and ERR interrupts while bus inactive
	}
}

/**
  * @brief  This function handles I2C1 Error interrupt request.
  * @param  None
  * @retval : None
  * Note: The error and event handlers must be in the same priority group. Other interrupts may be in the group, but must be lower priority
  * ER must have the highest priority in the group (but not necessarily on the device - Method2 from ref manual)
  */
void I2C1_ER_IRQHandler(void) {
	__IO uint32_t SR1Register, SR2Register;
	/* Read the I2C1 status register */
	SR1Register = I2C1->SR1;
	if(SR1Register & 0x0F00) {	//an error
		I2C1error.error=((SR1Register&0x0F00)>>8);//save error
		I2C1error.job=job;	//the task
	}
	/* If AF, BERR or ARLO, abandon the current job and commence new if there are jobs*/
	if(SR1Register & 0x0700) {
		SR2Register = I2C1->SR2;//read second status register to clear ADDR if it is set (note that BTF will not be set after a NACK)
		I2C_ITConfig(I2C1, I2C_IT_BUF, DISABLE);//disable the RXNE/TXE interrupt - prevent the ISR tailchaining onto the ER (hopefully)
		Jobs&=~(0x00000001<<job);//cancel the current job - abandoned
		if(Jobs) {		//ensure start of a new job if there are still jobs left
			if(!(I2C1->CR1&0x0100)) {//if we are not already sending a start
				uint16_t timeout=50000;
				while(I2C1->CR1&0x0200 && timeout){timeout++;}//wait for any stop to finish sending
				if(!timeout) {//timeout error - save the job (this means hardware is really screwed, but at least we dont lock up processor)
					I2C1error.error|=0x40;
					I2C1error.job=job;
				}
				I2C_GenerateSTART(I2C1,ENABLE);//sets a start
			}
			I2C_ITConfig(I2C1, I2C_IT_EVT|I2C_IT_ERR, ENABLE);//Ensure EVT and ERR interrupts enabled 
		}
		else if(!(SR1Register & 0x0200) && !(I2C1->CR1&0x0200)) {//if we dont have an ARLO error, ensure sending of a stop
			if(I2C1->CR1&0x0100) {//We are currently trying to send a start, this is very bad as start,stop will hang the peripheral
				uint16_t timeout=50000;
				while(I2C1->CR1&0x0100 && timeout){timeout++;}//wait for any start to finish sending
				I2C_GenerateSTOP(I2C1,ENABLE);//send stop to finalise bus transaction
				timeout=50000;
				while(I2C1->CR1&0x0200 && timeout){timeout++;}//wait for stop to finish sending
				if(!timeout) {//timeout error - save the job (this means hardware is really screwed, but at least we dont lock up processor)
					I2C1error.error|=0x80;
					I2C1error.job=job;
				}
				I2C_Config();//reset and configure the hardware						
			}
			else {
				I2C_GenerateSTOP(I2C1,ENABLE);//stop to free up the bus
				I2C_ITConfig(I2C1, I2C_IT_EVT|I2C_IT_ERR, DISABLE);//Disable EVT and ERR interrupts while bus inactive
			}
		}
	}
	I2C1->SR1 &=~0x0F00;		//reset all the error bits to clear the interrupt
}

/**
  * @brief  This function sets a job as requested on I2C1
  * @param : job number
  * @retval : None
  */
void I2C1_Request_Job(uint8_t job_) {
	if(job_<32) {			//sanity check
		Jobs|=1<<job_;		//set the job bit, do it here and use interrupt flag to detect bus inactive in case of I2C interrupting here
		if(!((I2C1->CR2)&(I2C_IT_EVT))) {//if we are restarting the driver
			if(!(I2C1->CR1&0x0100)) {// ensure sending a start
				uint16_t timeout=50000;
				while(I2C1->CR1&0x0200 && timeout){timeout++;}//wait for any stop to finish sending
				if(!timeout) {//timeout error - save the job
					I2C1error.error=0x10;
					I2C1error.job=job_;
					return;
				}
				if(!(I2C1->CR1&0x0100))//check nothing has changed here
					I2C_GenerateSTART(I2C1,ENABLE);//send the start for the new job
			}
			I2C_ITConfig(I2C1, I2C_IT_EVT|I2C_IT_ERR, ENABLE);//allow the interrupts to fire off again
		}
	}
}

/**
  * @brief  This function sets the data pointer on a job
  * @param : job number, pointer to data
  * @retval : None
  */
void I2C1_Setup_Job(uint8_t job_, volatile uint8_t* data) {
	if(job_<I2C_NUMBER_JOBS)
		I2C_jobs[job_].data_pointer=data;
}

/**
  * @brief  Configures the I2C1 interface
  * @param  None
  * @retval None
  */
void I2C_Config() {			//Configure I2C1 for the sensor bus
	I2C_InitTypeDef I2C_InitStructure;
	I2C_InitStructure.I2C_Mode = I2C_Mode_I2C;
	I2C_InitStructure.I2C_DutyCycle = I2C_DutyCycle_2;
	I2C_InitStructure.I2C_OwnAddress1 = 0xAD;//0xAM --> ADAM
	I2C_InitStructure.I2C_Ack = I2C_Ack_Enable;
	I2C_InitStructure.I2C_AcknowledgedAddress= I2C_AcknowledgedAddress_7bit;
	I2C_InitStructure.I2C_ClockSpeed = 200000;
	//Setup the pointers to the read data
	I2C1_Setup_Job(L3GD20_STATUS, (volatile uint8_t*)L3GD20_Data_Buffer);//Gyro data buffer
	I2C1_Setup_Job(L3GD20_READ, (volatile uint8_t*)&(L3GD20_Data_Buffer[2]));//Gyro data buffer, index to the mems data
	I2C1_Setup_Job(AFROESC_READ, (volatile uint8_t*)AFROESC_Data_Buffer);//ESC data buffer
	I2C1_Setup_Job(AFROESC_THROTTLE, (volatile uint8_t*)&AFROESC_Throttle);//ESC throttle
	//Assert the bus
	GPIO_SetBits(GPIOB,I2C1_SDA|I2C1_SCL);//Set bus high, make sure GPIO in correct state to start with
	GPIO_InitTypeDef	GPIO_InitStructure;
	GPIO_InitStructure.GPIO_Pin = I2C1_SCL|I2C1_SDA;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init( GPIOB, &GPIO_InitStructure );//Configure the pins as output open drain so we can clk them as GPIO
	GPIO_SetBits(GPIOB,I2C1_SDA|I2C1_SCL);//Set bus high
        /* Wait for any clock stretching to finish - this has a timeout of 2.55ms*/
	uint8_t count=255;
        while (!GPIO_ReadInputDataBit(GPIOB,I2C1_SCL)&&count) {
	        Delay(10);
		count--;
	}
	/* Generate a start condition */
	GPIO_ResetBits(GPIOB,I2C1_SDA);//Set bus data low
	Delay(10);
 	GPIO_ResetBits(GPIOB,I2C1_SCL);//Set bus scl low
	Delay(10);
	GPIO_SetBits(GPIOB,I2C1_SDA);//Set bus data high
	Delay(10);
 	GPIO_SetBits(GPIOB,I2C1_SCL);//Set bus scl high
	Delay(10);
	//Make sure the bus is free by clocking it until any slaves release the line - 8 clocks (-1 as we already generated parts of one pulse)
	for(uint8_t n=0;n<7;n++) {
        	/* Wait for any clock stretching to finish - this has a timeout of 2.55ms*/
		count=255;
        	while (!GPIO_ReadInputDataBit(GPIOB,I2C1_SCL)&&count) {
		        Delay(10);
			count--;
		}
		/* Pull low */
		GPIO_ResetBits(GPIOB,I2C1_SCL);//Set bus low
		Delay(10);
		/* Release high again */
		GPIO_SetBits(GPIOB,I2C1_SCL);//Set bus high
		Delay(10);
	}
        /* Wait for any clock stretching to finish - this has a timeout of 2.55ms*/
	count=255;
        while (!GPIO_ReadInputDataBit(GPIOB,I2C1_SCL)&&count) {
	        Delay(10);
		count--;
	}
	/* Pull low - the slave should now release for NACK (no devices allowed at 0xFF)*/
	GPIO_ResetBits(GPIOB,I2C1_SCL);//Set bus low, end of 8th clock pulse
	Delay(10);
	/* Generate a stop condition */
 	GPIO_ResetBits(GPIOB,I2C1_SDA);//Set bus sda low
	Delay(10);
 	GPIO_SetBits(GPIOB,I2C1_SCL);//Set bus scl high
	Delay(10);
 	GPIO_SetBits(GPIOB,I2C1_SDA);//Set bus sda high
	//Enable the hardware
	/*Enable the I2C1 clk*/
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);
	I2C_DeInit(I2C1);		//Deinit and reset the I2C to avoid it locking up
	I2C_SoftwareResetCmd(I2C1, ENABLE);
	I2C_SoftwareResetCmd(I2C1, DISABLE);
	I2C_ITConfig(I2C1, I2C_IT_EVT|I2C_IT_ERR, DISABLE);//Disable EVT and ERR interrupts - they are enabled by the first request
	I2C_Init( I2C1, &I2C_InitStructure );
	I2C_Cmd( I2C1, ENABLE );
	//Configure the hardware as alt function
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_OD;
	GPIO_Init( GPIOB, &GPIO_InitStructure );
}
