#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "stm32f10x.h"
#include "core_cm3.h"
#include "buffer.h"
#include "interrupts.h"

#define VCXO_FREQ 26000000UL

#define RSSI_THRESH -93 /*-93dBm RSSI, should be fine with ~100mW into a 14.5dBi yagi on the ground and 100km slant range*/
#define DEFAULT_POWER_LEVEL 32 /*gives ~ 15dBm at 3.3v*/
#define DEFAULT_SHIFT 200 /*200 hz tone sep*/
#define DEFAULT_FREQ 434075000UL /*carrier center at channel 0*/
#define DEFAULT_CHANNEL 600 /* 0.6kHz channel spacing */
#define DEFAULT_BPS 200 /*200bps for the uplink*/

#define RTTY_BAUD 50 /* This is pretty standard for balloons, means the PCLK1 has to be down at 3mhz and system clk at 24mhz*/

#define AFA_BAD_SHORTTIME 250 /* AFA (Active frequency avoidance) config: avoid channel if sync or preamble errors every 250ms or more with RSSI high*/
#define AFA_BAD_LONGTIME 2000 /* More than two seconds between interference and the channel will be judged usable */
#define AFA_CHANNELS 4 /* Four possible channels to avoid interference */
#define AFA_BAD_LIMIT 16 /* Sixteen bad events before the channel is changed */

#define SDN_LOW  GPIO_WriteBit(GPIOB,GPIO_Pin_9,Bit_RESET)
#define SDN_HIGH GPIO_WriteBit(GPIOB,GPIO_Pin_9,Bit_SET)

#define NSEL_LOW  GPIO_WriteBit(GPIOA,GPIO_Pin_4,Bit_RESET)
#define NSEL_HIGH GPIO_WriteBit(GPIOA,GPIO_Pin_4,Bit_SET)

#define GET_NIRQ GPIO_ReadInputDataBit(GPIOB,GPIO_Pin_0)
#define GET_CTS GPIO_ReadInputDataBit(GPIOB,GPIO_Pin_11)

#define SI_PORTB GPIO_Pin_9|GPIO_Pin_0|GPIO_Pin_10|GPIO_Pin_11
#define SI_PORTA GPIO_Pin_4|GPIO_Pin_5|GPIO_Pin_6|GPIO_Pin_7

#define USE_GPIO_CTS /*The spi wait ready function is not used - use the GPIO polling instead*/

/*Upper level state machine states*/
enum{DEFAULT_MODE=0,IRQ_MODE,AFC_HACK_MODE,READ_MODE,READ_COMPLETE_MODE,READ_RSSI_COMPLETED,TX_MODE,TX_COMPLETE_MODE};

extern volatile uint8_t Channel_rx,Channel_tx,Silabs_spi_state,Silabs_driver_state;
extern volatile int8_t Last_RSSI;

uint8_t send_string_to_silabs(uint8_t* str);
void add_to_silabs_buffer(uint8_t data);
uint8_t get_from_silabs_buffer(uint8_t* status);
uint8_t silabs_cts_jammed(void);
uint8_t silabs_state_machine_jammed(void);
extern uint8_t Silabs_Header[5];

void si446x_busy_wait_send_receive(uint8_t tx_bytes, uint8_t rx_bytes, uint8_t *tx_data, uint8_t *rx_data);
void si446x_spi_state_machine( volatile uint8_t *state_, uint8_t tx_bytes, uint8_t *tx_data, uint8_t rx_bytes, uint8_t *rx_data, void(*callback)(volatile uint8_t *, uint8_t));
void si446x_state_machine( volatile uint8_t *state_, uint8_t reason );
void si446x_set_modem(void);
void si446x_set_deviation_channel_bps(uint32_t deviation, uint32_t channel_space, uint32_t bps);
void si446x_set_frequency(uint32_t freq);
uint8_t si446x_setup(uint8_t* header);

