#include <string.h>
#include "errno_ex.h"
#include "stm32f4xx_hal.h"
#include "irq.h"
#include "dma.h"
#include "usart.h"
#include "usart_test.h"

#define UART_IO_BUFFER_SIZE	64

#define HUARTEX_DMA					(&huartex2_dma)
#define HUART_DMA						(&huartex2_dma.huart)

const char alphabet[] = "abcdefghijklmnopqrstuvwxyz";

static UARTEX_HandleTypeDef 	huartex2_pd5_pd6;	// originally used as polling mode test data
static UARTEX_HandleTypeDef 	huartex2_dma;
static DMAEX_HandleTypeDef 		hdmaex_rx_uart2;
static DMAEX_HandleTypeDef 		hdmaex_tx_uart2;
static IRQ_HandleTypeDef 			hirq_uart2_rxdma;
static IRQ_HandleTypeDef 			hirq_uart2_txdma;
static IRQ_HandleTypeDef			hirq_uart2;
static IRQ_HandleTypeDef			hirq_uart2_dma;
static GPIOEX_TypeDef					rxpin;
static GPIOEX_TypeDef					txpin;
static GPIOEX_TypeDef					rxpin_dma;
static GPIOEX_TypeDef					txpin_dma;

void refresh_static_variables(void)
{
	rxpin = PD6_As_Uart2Rx_Default;
	txpin = PD5_As_Uart2Tx_Default;
	hirq_uart2 = IRQ_Handle_Uart2_Default;
	
	UARTEX_Clone(&huartex2_pd5_pd6, &UARTEX_Handle_Uart2_Default, 
		&rxpin, &txpin, &hirq_uart2, NULL, NULL);
	
	rxpin_dma = PD6_As_Uart2Rx_Default;
	txpin_dma = PD5_As_Uart2Tx_Default;
	
	hirq_uart2_rxdma = IRQ_Handle_Uart2RxDMA_Default;
	hirq_uart2_txdma = IRQ_Handle_Uart2TxDMA_Default;
	
	hdmaex_rx_uart2 = DMAEX_Handle_Uart2Rx_Default;
	hdmaex_rx_uart2.hirq = &hirq_uart2_rxdma;
	
	hdmaex_tx_uart2 = DMAEX_Handle_Uart2Tx_Default;
	hdmaex_tx_uart2.hirq = &hirq_uart2_txdma;
	
	hirq_uart2_dma = IRQ_Handle_Uart2_Default;
	UARTEX_Clone(&huartex2_dma, 	&UARTEX_Handle_Uart2_Default, 
	&rxpin_dma, &txpin_dma, &hirq_uart2_dma, &hdmaex_rx_uart2, &hdmaex_tx_uart2);
}

///////////////////////////////////////////////////////////////////////////////
/** Analysis

1. what happens in initialization and what is the 'states' to be asserted?

IP Blocks

PORT: Clock, Config; No IRQ, No Enable
DMA: Clock, Config; Enable used but not in init; IRQ Handler, Config & Enable
UART: Clock, Config, Enable; IRQ Handler, Config & Enable

HAL Layers

UART: State, Error
DMA: State, Error

Initialization Sequence:
		Actions											Consequence								
		MX_GPIO_Init() 						
[x]		...												PORT Clock Enabled 
		MX_DMA_Init()							
[x]		...												DMA Clock Enabled
[x]		...												DMA IRQ Config & Enabled;
		MX_USART2_UART_Init() 
			HAL_UART_Init() 	
				HAL_UART_MspInit()			
[x]				...										UART Clock Enabled
[x]				HAL_GPIO_Init()				PORT(GPIO) Config
[x]				HAL_DMA_Init()				rx DMA Config, dma State -> READY
[x]				...										uart_rx/dma mutual reference
[x]				HAL_DMA_Init()				tx DMA Config, dma State -> READY
[x]				...										uart_tx/dma mutual reference
[x]				...										UART IRQ Config & Enabled
[x]			UART_SetConfig()				UART Config
[x]			...											uart State -> READY
[x]			...											UART Enabled

Init assertion:									reverted in DeInit?				potential power problem?
	port clock enabled						no												yes
	dma clock enabled							no												yes
	dma irq config & enabled			no												???
	dma handler exist							no												???
	uart clock enabled						yes												yes
	port configured	(not reset)		yes
	dma state -> READY 	x 2				yes
	rx/dma linked				x	2				no
	uart irq config & enabled			yes (config not cleared)
	uart configured								no
	uart state->READY							yes (config not cleared)
	uart enabled									no!
	
*******************************************************************************

Deinitialization Sequence:

		HAL_UART_DeInit()
			HAL_UART_MspDeInit()
[x]			...											UART Clock Disabled.
[x]			HAL_GPIO_DeInit()				PORT GPIO reset
				HAL_DMA_DeInit()				rxdma
[x]				...										Disable DMA, clear config/flags
[x]				...										dma State -> RESET
				HAL_DMA_DeInit()				txdma
[x]				...										Disable DMA, clear config/flags
[x]				...										dma State -> RESET
[x]			...											UART IRQ disabled
[x]		...												UART state -> RESET

Deinit Assertion:

		uart clock disabled.
		port gpio reset.
		rxdma State -> RESET
		txdma State -> RESET
		UART IRQ -> Disabled
		UART State -> RESET
	
******************************************************************************/	

///////////////////////////////////////////////////////////////////////////////
// internal functions (some may be obsolete)

bool gpio_modes_all_input(GPIO_TypeDef* gpiox, uint32_t pins)
{
	uint32_t pos;
	for (pos = 0; pos < 16; pos++)
	{
		if (((uint32_t)1 << pos) & pins)
		{
			if ((gpiox->MODER & (GPIO_MODER_MODER0 << (pos * 2))) != 0)
				return false;
		}
	}
	return true;
}

bool gpio_modes_all_noninput(GPIO_TypeDef* gpiox, uint32_t pins)
{
	uint32_t pos;
	for (pos = 0; pos < 16; pos++)
	{
		if (((uint32_t)1 << pos) & pins)
		{
			if ((gpiox->MODER & (GPIO_MODER_MODER0 << (pos * 2))) == 0)
				return false;
		}
	}
	return true;
}


bool dma1_clock_enabled(void) 
{	
// #define __DMA1_CLK_ENABLE()          (RCC->AHB1ENR |= (RCC_AHB1ENR_DMA1EN))
// #define __DMA2_CLK_ENABLE()          (RCC->AHB1ENR |= (RCC_AHB1ENR_DMA2EN))	
	return (RCC->AHB1ENR & RCC_AHB1ENR_DMA1EN) ? true : false;
}

bool dma2_clock_enabled(void) 
{
	return (RCC->AHB1ENR & RCC_AHB1ENR_DMA2EN) ? true : false;
}

bool irq_enabled(IRQn_Type IRQn) 
{
	uint32_t reg;
	
	/** or NVIC->ISER[(uint32_t)((int32_t)IRQn) >> 5]; **/
	reg = NVIC->ISER[(uint32_t)((int32_t)IRQn) >> 5];
	return (reg & (1 << ((uint32_t)(IRQn) & 0x1F))) ? true : false;
}


bool uart_mode_configured(UART_HandleTypeDef* huart)
{
	if (huart->Init.Mode == 0) return false;
	return ((huart->Instance->CR1 & (USART_CR1_TE | USART_CR1_RE)) == huart->Init.Mode) ? true : false;
}

bool uart_mode_not_configured(UART_HandleTypeDef* huart)
{
	return ((huart->Instance->CR1 & (USART_CR1_TE | USART_CR1_RE)) == 0) ? true : false;
}

bool uart_enabled(UART_HandleTypeDef* huart)
{
	/** ((__HANDLE__)->Instance->CR1 |=  USART_CR1_UE) **/	
	return (huart->Instance->CR1 & USART_CR1_UE) ? true : false;
}

bool __uart_dmat_enabled(UART_HandleTypeDef* huart)
{
	return (huart->Instance->CR3 & USART_CR3_DMAT) ? true : false;
}

bool __uart_dmar_enabled(UART_HandleTypeDef* huart)
{
	return (huart->Instance->CR3 & USART_CR3_DMAR) ? true : false;
}

bool __dma_sxcr_enabled(DMA_HandleTypeDef* hdma)
{
// #define __HAL_DMA_ENABLE(__HANDLE__)      ((__HANDLE__)->Instance->CR |=  DMA_SxCR_EN)
	return (hdma->Instance->CR & DMA_SxCR_EN) ? true : false;
}

bool uart_txdma_linked(UART_HandleTypeDef* huart, DMA_HandleTypeDef* tx)
{
	if (huart == 0) return false;
	if (tx == 0) return false;
	
	if (huart->hdmatx == 0) return false;
	if (huart->hdmatx != tx) return false;
	if (huart->hdmatx->Parent == 0) return false;
	if (huart->hdmatx->Parent != huart) return false;
		
	return true;
}

bool uart_rxdma_linked(UART_HandleTypeDef* huart, DMA_HandleTypeDef* rx)
{
	if (huart == 0) return false;
	if (rx == 0) return false;
	
	if (huart->hdmarx == 0) return false;
	if (huart->hdmarx != rx) return false;
	if (huart->hdmarx->Parent == 0) return false;
	if (huart->hdmarx->Parent != huart) return false;
	
	return true;
}




///////////////////////////////////////////////////////////////////////////////
// TEST GROUP BEGIN - MspInit

TEST_GROUP(Usart_DMA_MspInit);
TEST_SETUP(Usart_DMA_MspInit)
{
//	__USART2_FORCE_RESET();
//	__DMA1_FORCE_RESET();
//	
//	__USART2_RELEASE_RESET();
//	__DMA1_FORCE_RESET();

//	rxpin = PD6_As_Uart2Rx_Default;
//	txpin = PD5_As_Uart2Tx_Default;
//	hirq_uart2 = IRQ_Handle_Uart2_Default;
//	
//	UARTEX_Clone(&huartex2_pd5_pd6, &UARTEX_Handle_Uart2_Default, 
//		&rxpin, &txpin, &hirq_uart2, NULL, NULL);
//	
//	rxpin_dma = PD6_As_Uart2Rx_Default;
//	txpin_dma = PD5_As_Uart2Tx_Default;
//	
//	// DMAEX_Clone(&hdmaex_rx_uart2, &DMAEX_Handle_Uart2Rx_Default);
//	hirq_uart2_rxdma = IRQ_Handle_DMA1_Stream5_Default;
//	hirq_uart2_txdma = IRQ_Handle_DMA1_Stream6_Default;
//	
//	hdmaex_rx_uart2 = DMAEX_Handle_Uart2Rx_Default;
//	hdmaex_rx_uart2.hirq = &hirq_uart2_rxdma;
//	
//	hdmaex_tx_uart2 = DMAEX_Handle_Uart2Tx_Default;
//	hdmaex_tx_uart2.hirq = &hirq_uart2_txdma;
//	
//	hirq_uart2_dma = IRQ_Handle_Uart2_Default;
//	UARTEX_Clone(&huartex2_dma, 	&UARTEX_Handle_Uart2_Default, 
//		&rxpin_dma, &txpin_dma, 
//		&hirq_uart2_dma, &hdmaex_rx_uart2, &hdmaex_tx_uart2);

	refresh_static_variables();
}

TEST_TEAR_DOWN(Usart_DMA_MspInit)
{
}
	

TEST(Usart_DMA_MspInit, InitUartClockShouldBeEnabled)
{
	UART_HandleTypeDef* h = &huartex2_pd5_pd6.huart;
	HAL_UART_MspInit(h);
	TEST_ASSERT_TRUE(HAL_UART_ClockIsEnabled(h->Instance));
}


TEST(Usart_DMA_MspInit, InitRxPinGpioShouldBeInitialized)
{
	UARTEX_HandleTypeDef* hue = &huartex2_pd5_pd6;
	
	HAL_UART_MspInit(&hue->huart);
	TEST_ASSERT_EQUAL(GPIOEX_STATE_SET, hue->rxpin->state);
}

TEST(Usart_DMA_MspInit, InitTxPinGpioShouldBeInitialized)
{
	UARTEX_HandleTypeDef* hue = &huartex2_pd5_pd6;
	
	HAL_UART_MspInit(&hue->huart);
	TEST_ASSERT_EQUAL(GPIOEX_STATE_SET, hue->txpin->state);
}

TEST(Usart_DMA_MspInit, InitRxDMAShouldBeLinked)
{
	HAL_UART_MspInit(HUART_DMA);
	TEST_ASSERT_TRUE(uart_rxdma_linked(HUART_DMA, &HUARTEX_DMA->hdmaex_rx->hdma));
}

TEST(Usart_DMA_MspInit, InitRxDMAEXShouldBeInitialized)
{
	HAL_UART_MspInit(HUART_DMA);
	TEST_ASSERT_EQUAL(DMAEX_HANDLE_STATE_SET, HUARTEX_DMA->hdmaex_rx->state);
}


TEST(Usart_DMA_MspInit, InitTxDMAShouldBeLinked)
{
	HAL_UART_MspInit(HUART_DMA);
	TEST_ASSERT_TRUE(uart_txdma_linked(HUART_DMA, &HUARTEX_DMA->hdmaex_tx->hdma));
}

TEST(Usart_DMA_MspInit, InitTxDMAEXShouldBeInitialized)
{
	HAL_UART_MspInit(HUART_DMA);
	TEST_ASSERT_EQUAL(DMAEX_HANDLE_STATE_SET, HUARTEX_DMA->hdmaex_tx->state);
}

TEST(Usart_DMA_MspInit, InitUartIRQHandleShouldBeInitialized)
{
	HAL_UART_MspInit(HUART_DMA);
	TEST_ASSERT_EQUAL(IRQ_HANDLE_STATE_SET, HUARTEX_DMA->hirq->state);
}

TEST(Usart_DMA_MspInit, DeInitUartIRQHandleShouldBeDeInitialized)
{
	HAL_UART_MspInit(HUART_DMA);
	HAL_UART_MspDeInit(HUART_DMA);
	TEST_ASSERT_EQUAL(IRQ_HANDLE_STATE_RESET, HUARTEX_DMA->hirq->state);
}

TEST(Usart_DMA_MspInit, DeInitTxDMAEXShouldBeDeInitialized)
{
	HAL_UART_MspInit(HUART_DMA);
	HAL_UART_MspDeInit(HUART_DMA);
	TEST_ASSERT_EQUAL(DMAEX_HANDLE_STATE_RESET, HUARTEX_DMA->hdmaex_tx->state);
}

TEST(Usart_DMA_MspInit, DeInitTxDMAShouldBeUnlinked)
{
	HAL_UART_MspInit(HUART_DMA);
	HAL_UART_MspDeInit(HUART_DMA);
	TEST_ASSERT_NULL(HUART_DMA->hdmatx);
	TEST_ASSERT_NULL(HUARTEX_DMA->hdmaex_tx->hdma.Parent);
}

TEST(Usart_DMA_MspInit, DeInitRxDMAEXShouldBeDeInitialized)
{
	HAL_UART_MspInit(HUART_DMA);
	HAL_UART_MspDeInit(HUART_DMA);
	TEST_ASSERT_EQUAL(DMAEX_HANDLE_STATE_RESET, HUARTEX_DMA->hdmaex_rx->state);
}

TEST(Usart_DMA_MspInit, DeInitRxDMAShouldBeUnlinked)
{
	HAL_UART_MspInit(HUART_DMA);
	HAL_UART_MspDeInit(HUART_DMA);
	TEST_ASSERT_NULL(HUART_DMA->hdmarx);
	TEST_ASSERT_NULL(HUARTEX_DMA->hdmaex_rx->hdma.Parent);
}

TEST(Usart_DMA_MspInit, DeInitRxTxPinGpioShouldBeDeInitialized)
{
	HAL_UART_MspInit(HUART_DMA);
	HAL_UART_MspDeInit(HUART_DMA);
	TEST_ASSERT_EQUAL(GPIOEX_STATE_RESET, HUARTEX_DMA->rxpin->state);
	TEST_ASSERT_EQUAL(GPIOEX_STATE_RESET, HUARTEX_DMA->txpin->state);
}

TEST_GROUP_RUNNER(Usart_DMA_MspInit)
{
	RUN_TEST_CASE(Usart_DMA_MspInit, InitUartClockShouldBeEnabled);
	RUN_TEST_CASE(Usart_DMA_MspInit, InitRxPinGpioShouldBeInitialized);
	RUN_TEST_CASE(Usart_DMA_MspInit, InitTxPinGpioShouldBeInitialized);
	RUN_TEST_CASE(Usart_DMA_MspInit, InitRxDMAShouldBeLinked);
	RUN_TEST_CASE(Usart_DMA_MspInit, InitRxDMAEXShouldBeInitialized);
	RUN_TEST_CASE(Usart_DMA_MspInit, InitTxDMAShouldBeLinked);
	RUN_TEST_CASE(Usart_DMA_MspInit, InitTxDMAEXShouldBeInitialized);
	RUN_TEST_CASE(Usart_DMA_MspInit, InitUartIRQHandleShouldBeInitialized);

	RUN_TEST_CASE(Usart_DMA_MspInit, DeInitUartIRQHandleShouldBeDeInitialized);
	RUN_TEST_CASE(Usart_DMA_MspInit, DeInitTxDMAEXShouldBeDeInitialized);
	RUN_TEST_CASE(Usart_DMA_MspInit, DeInitTxDMAShouldBeUnlinked);
	RUN_TEST_CASE(Usart_DMA_MspInit, DeInitRxDMAEXShouldBeDeInitialized);
	RUN_TEST_CASE(Usart_DMA_MspInit, DeInitRxDMAShouldBeUnlinked);
	RUN_TEST_CASE(Usart_DMA_MspInit, DeInitRxTxPinGpioShouldBeDeInitialized);
}


///////////////////////////////////////////////////////////////////////////////
//
/***********	The following cases are Learning Tests **************************

	USART2 tx/rx short required to run these tests.

******************************************************************************/

TEST_GROUP(LT_Usart_DMA_TxRx);
TEST_SETUP(LT_Usart_DMA_TxRx)
{
//	hirq_uart2_rxdma = IRQ_Handle_DMA1_Stream5_Default;
//	hirq_uart2_txdma = IRQ_Handle_DMA1_Stream6_Default;
//	
//	hdmaex_rx_uart2 = DMAEX_Handle_Uart2Rx_Default;
//	hdmaex_rx_uart2.hirq = &hirq_uart2_rxdma;
//	
//	hdmaex_tx_uart2 = DMAEX_Handle_Uart2Tx_Default;
//	hdmaex_tx_uart2.hirq = &hirq_uart2_txdma;
//	
//	hirq_uart2_dma = IRQ_Handle_Uart2_Default;
//	
//	UARTEX_Clone(&huartex2_dma, 	&UARTEX_Handle_Uart2_Default, 
//		&PD6_As_Uart2Rx_Default, &PD5_As_Uart2Tx_Default, 
//		&hirq_uart2_dma, &hdmaex_rx_uart2, &hdmaex_tx_uart2);
	
	refresh_static_variables();
	
	HAL_UART_Init(HUART_DMA);
}

TEST_TEAR_DOWN(LT_Usart_DMA_TxRx)
{
	HAL_UART_DMAStop(HUART_DMA);
	HAL_UART_DeInit(HUART_DMA);
}


void loopBackSixBytesDMA(void) 	// reuse this op to demonstrate the usart is fine after certain operation 
{
		
	char rx[7];
	char tx[7];
	char token[6] = "@#$%^&";

	/** prepare **/
	memset(rx, 0, 7);
	memset(tx, 0, 7);
	memmove(tx, token, strlen(token));
	
	TEST_ASSERT_EQUAL(HAL_UART_STATE_READY, HUART_DMA->State);
	TEST_ASSERT_EQUAL(HAL_UART_ERROR_NONE, HUART_DMA->ErrorCode);
	
	/** recv **/
	HAL_UART_Receive_DMA(HUART_DMA, (uint8_t*)rx, strlen(tx));
	TEST_ASSERT_EQUAL(HAL_UART_STATE_BUSY_RX, HUART_DMA->State);
	
	/** send **/
	HAL_UART_Transmit_DMA(HUART_DMA, (uint8_t*)tx, strlen(tx));
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_BUSY_TX_RX, HUART_DMA->State);	/** this may fail if compiler optimization level low **/
	
	/** wait and check **/
	HAL_Delay(100);
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_READY, HUART_DMA->State);
	TEST_ASSERT_EQUAL(HAL_UART_ERROR_NONE, HUART_DMA->ErrorCode);
	TEST_ASSERT_EQUAL_STRING(token, rx);	
}

TEST(LT_Usart_DMA_TxRx, LoopBackSixBytesTwice)
{
	loopBackSixBytesDMA();
	loopBackSixBytesDMA();
}

TEST(LT_Usart_DMA_TxRx, ReceiveMultipleBytesInWaitStopCheckManner)
{
	char rx[64];
	char tx[64];
	
	/** prepare **/
	memset(rx, 0, sizeof(rx));
	memset(tx, 0, sizeof(tx));
	snprintf(tx, sizeof(tx),  "i love test driven development.");
	TEST_ASSERT_EQUAL(HAL_UART_STATE_READY, HUART_DMA->State);

	/** receive **/
	HAL_UART_Receive_DMA(HUART_DMA, (uint8_t*)rx, sizeof(rx));
	TEST_ASSERT_EQUAL(HAL_UART_STATE_BUSY_RX, HUART_DMA->State);
	
	/** send **/
	HAL_UART_Transmit_DMA(HUART_DMA, (uint8_t*)tx, strlen(tx));
	TEST_ASSERT_EQUAL(HAL_UART_STATE_BUSY_TX_RX, HUART_DMA->State);
	
	/** wait **/
	HAL_Delay(100);
	TEST_ASSERT_EQUAL(HAL_UART_STATE_BUSY_RX, HUART_DMA->State);
	TEST_ASSERT_EQUAL(sizeof(rx) - strlen(tx), __HAL_DMA_GET_COUNTER(HUART_DMA->hdmarx));
	TEST_ASSERT_EQUAL_STRING(tx, rx);
		
	/** 
	#define __HAL_UART_ENABLE(__HANDLE__)               ((__HANDLE__)->Instance->CR1 |=  USART_CR1_UE)
	#define __HAL_UART_DISABLE(__HANDLE__)              ((__HANDLE__)->Instance->CR1 &=  ~USART_CR1_UE) 
	**/		
	
	/** stop **/
	HAL_UART_DMAStop(HUART_DMA);
	TEST_ASSERT_FALSE(HUART_DMA->Instance->CR1 & USART_CR1_UE);
	__HAL_UART_ENABLE(HUART_DMA);		/** <-- fix the problem **/
	TEST_ASSERT_TRUE(HUART_DMA->Instance->CR1 & USART_CR1_UE);
	
	TEST_ASSERT_EQUAL(HAL_UART_STATE_READY, HUART_DMA->State);
	TEST_ASSERT_EQUAL(sizeof(rx) - strlen(tx), __HAL_DMA_GET_COUNTER(HUART_DMA->hdmarx));

	loopBackSixBytesDMA();
}

TEST(LT_Usart_DMA_TxRx, ReceiveMultipleBytesInSuspendResumeManner)
{
	char rx[64];
	char tx[64];
	char fill[64];
	int	recv_cnt;
	uint32_t timestamp, timestamp2;
	
	/** prepare **/
	recv_cnt = 0;
	memset(rx, 0, sizeof(rx));
	memset(fill, 0, sizeof(fill));
	snprintf(tx, sizeof(tx), "i love test driven development. i love test driven development");
	TEST_ASSERT_EQUAL(HAL_UART_STATE_READY, HUART_DMA->State);
	
	/** receive **/
	HAL_UART_Receive_DMA(HUART_DMA, (uint8_t*)rx, sizeof(rx));
	TEST_ASSERT_EQUAL(HAL_UART_STATE_BUSY_RX, HUART_DMA->State);

	/** send **/
	HAL_UART_Transmit_DMA(HUART_DMA, (uint8_t*)tx, strlen(tx));
	TEST_ASSERT_EQUAL(HAL_UART_STATE_BUSY_TX_RX, HUART_DMA->State);

	/** timeout to prevent dead loop **/
	timestamp = HAL_GetTick();
	for (;;) {
		
		if (HAL_GetTick() - timestamp > 5000) {	// fail occasionally, don't know why. change this value and
																						// recompile probably fix the problem.
			TEST_FAIL_MESSAGE(fill);
			return;
		}
		
		if (__HAL_DMA_GET_COUNTER(HUART_DMA->hdmarx) == sizeof(rx)) 
			continue;
		
		/** suspend, disable tc intr before clear EN bit **/
		HAL_UART_DMAPause(HUART_DMA);
		__HAL_DMA_DISABLE_IT(HUART_DMA->hdmarx, DMA_IT_TC);
		__HAL_DMA_DISABLE(HUART_DMA->hdmarx);	/** this clear DMA_SxCR_EN bit**/
		
		/* timeout to prevent dead loop **/
		timestamp2 = HAL_GetTick();
		for (;;) {
			
			if (HAL_GetTick() - timestamp2 > 100) {
				TEST_FAIL_MESSAGE("TIMEOUT when receiving data.");
				return;
			}
			
			if ((HUART_DMA->hdmarx->Instance->CR & DMA_SxCR_EN) == 0)
				break;
		}
		
		recv_cnt = sizeof(rx) - __HAL_DMA_GET_COUNTER(HUART_DMA->hdmarx);

		/** prevent buffer overflow **/
		TEST_ASSERT_TRUE(strlen(fill) + strlen(rx) < sizeof(fill) - 1);
		strcat(fill, rx);
		memset(rx, 0, recv_cnt);
		
		/** not necessarily, just prove the register writable now **/
		HUART_DMA->hdmarx->Instance->M0AR = (uint32_t)rx;			
		/** not necessarily if previous value reused **/
		__HAL_DMA_SET_COUNTER(HUART_DMA->hdmarx, sizeof(rx));

		/** clear tc flag and resume **/
		__HAL_DMA_CLEAR_FLAG(HUART_DMA->hdmarx, __HAL_DMA_GET_TC_FLAG_INDEX(HUART_DMA->hdmarx));
		__HAL_DMA_ENABLE(HUART_DMA->hdmarx);
		__HAL_DMA_ENABLE_IT(HUART_DMA->hdmarx, DMA_IT_TC);
		HAL_UART_DMAResume(HUART_DMA);
		
		/** make sure fill IS a string **/
		TEST_ASSERT_TRUE(0 == fill[sizeof(fill) - 1]);
		if (0 == strcmp(fill, tx)) {
			break;
		}
	}
}


static void receiveBufferOverflow(int rx_buf_size, int by_num) {

	char rx[64];

	/** prepare **/
	memset(rx, 0, sizeof(rx));
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_READY, HUART_DMA->State);

	/** start receive **/
	HAL_UART_Receive_DMA(HUART_DMA, (uint8_t*)rx, rx_buf_size);
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_BUSY_RX, HUART_DMA->State);
		
	/** send and overflow **/
	HAL_UART_Transmit_DMA(HUART_DMA, (uint8_t*)alphabet, rx_buf_size + by_num);
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_BUSY_TX_RX, HUART_DMA->State);
		
	/** wait **/
	HAL_Delay(200);
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_READY, HUART_DMA->State);	/** state ready 				**/
	TEST_ASSERT_EQUAL(0, __HAL_DMA_GET_COUNTER(HUART_DMA->hdmarx));	/** counter reg reach 0 **/
	TEST_ASSERT_TRUE(HUART_DMA->Instance->CR1 & USART_CR1_UE);				/** uart not disabled 	**/
	TEST_ASSERT_EQUAL_MEMORY(alphabet, rx, rx_buf_size);				/** mem equal						**/	
}

TEST(LT_Usart_DMA_TxRx, ReceiveMultipleBytesAfterRxOverflowByZero)
{
	uint32_t usart_sr;
	
	char rx[64];

	/** prepare **/
	memset(rx, 0, sizeof(rx));
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_READY, HUART_DMA->State);

	/** start receive **/
	HAL_UART_Receive_DMA(HUART_DMA, (uint8_t*)rx, 8 /** rx_buf_size **/);
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_BUSY_RX, HUART_DMA->State);
		
	/** send and overflow **/
	HAL_UART_Transmit_DMA(HUART_DMA, (uint8_t*)alphabet, 8 /** rx_buf_size + by_num **/);
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_BUSY_TX_RX, HUART_DMA->State);
		
	/** wait **/
	HAL_Delay(200);
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_READY, HUART_DMA->State);	/** state ready 				**/
	TEST_ASSERT_EQUAL(0, __HAL_DMA_GET_COUNTER(HUART_DMA->hdmarx));	/** counter reg reach 0 **/
	TEST_ASSERT_TRUE(HUART_DMA->Instance->CR1 & USART_CR1_UE);				/** uart not disabled 	**/
	TEST_ASSERT_EQUAL_MEMORY(alphabet, rx, 8 /* rx_buf_size */);				/** mem equal						**/		
	
	usart_sr = HUART_DMA->Instance->SR;
	TEST_ASSERT_FALSE(usart_sr & USART_SR_ORE);		/* no overrun */
	TEST_ASSERT_FALSE(usart_sr & USART_SR_RXNE);	/* no rx data */
}

TEST(LT_Usart_DMA_TxRx, ReceiveMultipleBytesAfterRxOverflowByOne)
{
	char chr;
	int buf_size = 8;
	uint32_t usart_sr;
	
	receiveBufferOverflow(buf_size, 1);
	
	usart_sr = HUART_DMA->Instance->SR;
	TEST_ASSERT_FALSE(usart_sr & USART_SR_ORE);		/* no overrun 					*/
	TEST_ASSERT_TRUE(usart_sr & USART_SR_RXNE);		/* has rx data in DR 		*/
	
	chr = 0;
	HAL_UART_Receive_DMA(HUART_DMA, (uint8_t*)&chr, 1);
	
	HAL_Delay(10);
	TEST_ASSERT_EQUAL(HAL_UART_STATE_READY, HUART_DMA->State);				/** state ready 				**/
	TEST_ASSERT_EQUAL(0, __HAL_DMA_GET_COUNTER(HUART_DMA->hdmarx));	/** counter reg reach 0 **/
	
	usart_sr = HUART_DMA->Instance->SR;
	TEST_ASSERT_FALSE(usart_sr & USART_SR_ORE);		/* overrun flag cleared */
	TEST_ASSERT_FALSE(usart_sr & USART_SR_RXNE);	/* no rx data now 			*/
	TEST_ASSERT_EQUAL(alphabet[buf_size], chr);
}

TEST(LT_Usart_DMA_TxRx, ReceiveMultipleBytesAfterRxOverflowByTwo)
{
	char chr;
	int buf_size = 8;
	uint32_t usart_sr;
	
	receiveBufferOverflow(buf_size, 2);
	
	usart_sr = HUART_DMA->Instance->SR;
	TEST_ASSERT_TRUE(usart_sr & USART_SR_ORE);		/* has overrun 					*/
	TEST_ASSERT_TRUE(usart_sr & USART_SR_RXNE);		/* has rx data in DR 		*/
	
	chr = 0;
	HAL_UART_Receive_DMA(HUART_DMA, (uint8_t*)&chr, 1);
	
	HAL_Delay(10);
	TEST_ASSERT_EQUAL(HAL_UART_STATE_READY, HUART_DMA->State);				/** state ready 				**/
	TEST_ASSERT_EQUAL(0, __HAL_DMA_GET_COUNTER(HUART_DMA->hdmarx));	/** counter reg reach 0 **/
	
	usart_sr = HUART_DMA->Instance->SR;
	TEST_ASSERT_FALSE(usart_sr & USART_SR_ORE);		/* overrun flag cleared */
	TEST_ASSERT_FALSE(usart_sr & USART_SR_RXNE);	/* no rx data now 			*/
	TEST_ASSERT_EQUAL(alphabet[buf_size], chr);		
}


TEST_GROUP_RUNNER(LT_Usart_DMA_TxRx)
{
	RUN_TEST_CASE(LT_Usart_DMA_TxRx, LoopBackSixBytesTwice);
	RUN_TEST_CASE(LT_Usart_DMA_TxRx, ReceiveMultipleBytesInWaitStopCheckManner);
	RUN_TEST_CASE(LT_Usart_DMA_TxRx, ReceiveMultipleBytesInSuspendResumeManner);
	RUN_TEST_CASE(LT_Usart_DMA_TxRx, ReceiveMultipleBytesAfterRxOverflowByZero);
	RUN_TEST_CASE(LT_Usart_DMA_TxRx, ReceiveMultipleBytesAfterRxOverflowByOne);
	RUN_TEST_CASE(LT_Usart_DMA_TxRx, ReceiveMultipleBytesAfterRxOverflowByTwo);
}


/////////////////////////////////////
//

TEST_GROUP(UARTEX_Handle);
TEST_SETUP(UARTEX_Handle){}
TEST_TEAR_DOWN(UARTEX_Handle){}
	
TEST(UARTEX_Handle, Ctor)
{
	UARTEX_HandleTypeDef* h;
	GPIOEX_TypeDef rx = PD6_As_Uart2Rx_Default;
	GPIOEX_TypeDef tx = PD5_As_Uart2Tx_Default;
	DMAEX_HandleTypeDef hdmaex_rx;
	DMAEX_HandleTypeDef	hdmaex_tx;
	IRQ_HandleTypeDef	hirq;
	
	h = UARTEX_Handle_Ctor(USART2, &UART_Handle_Uart2_Default.Init, &rx, &tx, &hdmaex_rx, &hdmaex_tx, &hirq);
	
	TEST_ASSERT_NOT_NULL(h);
	TEST_ASSERT_EQUAL_HEX32(&rx, h->rxpin);
	TEST_ASSERT_EQUAL_HEX32(&tx, h->txpin);
	TEST_ASSERT_EQUAL(USART2, h->huart.Instance);
	TEST_ASSERT_EQUAL_MEMORY(&UART_Handle_Uart2_Default.Init, &h->huart.Init, sizeof(UART_InitTypeDef));
	TEST_ASSERT_EQUAL_HEX32(&hdmaex_rx, h->hdmaex_rx);
	TEST_ASSERT_EQUAL_HEX32(&hdmaex_tx, h->hdmaex_tx);
	TEST_ASSERT_EQUAL_HEX32(&hirq, h->hirq);
	
	if (h) free(h);
}

TEST(UARTEX_Handle, Dtor)
{
	UARTEX_HandleTypeDef* h;
	GPIOEX_TypeDef rx = PD6_As_Uart2Rx_Default;
	GPIOEX_TypeDef tx = PD5_As_Uart2Tx_Default;
	DMAEX_HandleTypeDef hdmaex_rx;
	DMAEX_HandleTypeDef	hdmaex_tx;
	IRQ_HandleTypeDef	hirq;
	
	h = UARTEX_Handle_Ctor(USART2, &UART_Handle_Uart2_Default.Init, &rx, &tx, &hdmaex_rx, &hdmaex_tx, &hirq);
	if (h) UARTEX_Handle_Dtor(h);
}

TEST(UARTEX_Handle, FactoryCreate)
{
	UARTEX_HandleTypeDef* h;
	UARTEX_Handle_FactoryTypeDef factory;
	DMAEX_Handle_FactoryTypeDef	dma_factory;
	
	factory.dma_clk = &DMA_ClockProvider;
	factory.gpio_clk = &GPIO_ClockProvider;
	factory.registry = &IRQ_HandlerObjectRegistry;
	
	dma_factory.clk = factory.dma_clk;
	dma_factory.reg = factory.registry;
	
	const UART_HandleTypeDef* huart = &UART_Handle_Uart2_Default;
	const GPIOEX_TypeDef* rxpin = &PD6_As_Uart2Rx_Default;
	const GPIOEX_TypeDef* txpin = &PD5_As_Uart2Tx_Default;
	const DMA_HandleTypeDef* hdmarx = &DMA_Handle_Uart2Rx_Default;
	const IRQ_HandleTypeDef* hirq_dmarx = &IRQ_Handle_Uart2RxDMA_Default;
	const DMA_HandleTypeDef* hdmatx = &DMA_Handle_Uart2Tx_Default;
	const IRQ_HandleTypeDef* hirq_dmatx = &IRQ_Handle_Uart2TxDMA_Default;
	const IRQ_HandleTypeDef* hirq_uart = &IRQ_Handle_Uart2_Default;
	
	h = UARTEX_Handle_FactoryCreate(&factory, huart, rxpin, txpin, hdmarx, hirq_dmarx, hdmatx, hirq_dmatx, hirq_uart);
	
	TEST_ASSERT_NOT_NULL(h);
	if (h) {
		
		TEST_ASSERT_NOT_NULL(h->hdmaex_rx);
		if (h->hdmaex_rx)
		{
			TEST_ASSERT_EQUAL_HEX32(hdmarx->Instance, h->hdmaex_rx->hdma.Instance);
			TEST_ASSERT_EQUAL(hirq_dmarx->irqn, h->hdmaex_rx->hirq->irqn);
		}
		
		TEST_ASSERT_NOT_NULL(h->hdmaex_tx);
		if (h->hdmaex_tx)
		{
			TEST_ASSERT_EQUAL_HEX32(hdmatx->Instance, h->hdmaex_tx->hdma.Instance);
			TEST_ASSERT_EQUAL(hirq_dmatx->irqn, h->hdmaex_tx->hirq->irqn);
		}
		
		TEST_ASSERT_NOT_NULL(h->rxpin);
		
		if (h->rxpin)
		{
			TEST_ASSERT_EQUAL(rxpin->instance, h->rxpin->instance);
			TEST_ASSERT_EQUAL(rxpin->init.Pin, h->rxpin->init.Pin);
		}
		
		TEST_ASSERT_NOT_NULL(h->txpin);
		if (h->txpin)
		{
			TEST_ASSERT_EQUAL(txpin->instance, h->txpin->instance);
			TEST_ASSERT_EQUAL(txpin->init.Pin, h->txpin->init.Pin);
		}
		
		TEST_ASSERT_NOT_NULL(h->hirq);
		if (h->hirq)
		{
			TEST_ASSERT_EQUAL(hirq_uart->irqn, h->hirq->irqn);
		}
		
		TEST_ASSERT_EQUAL(huart->Instance, h->huart.Instance);
		
	
		DMAEX_Handle_FactoryDestroy(&dma_factory, h->hdmaex_rx);
		DMAEX_Handle_FactoryDestroy(&dma_factory, h->hdmaex_tx);
		GPIOEX_Dtor(h->rxpin);
		GPIOEX_Dtor(h->txpin);
		IRQ_Handle_Dtor(h->hirq);
		
		free(h);
	}
}

TEST(UARTEX_Handle, FactoryDestroy)
{
	UARTEX_HandleTypeDef* h;
	UARTEX_Handle_FactoryTypeDef factory;
	
	factory.dma_clk = &DMA_ClockProvider;
	factory.gpio_clk = &GPIO_ClockProvider;
	factory.registry = &IRQ_HandlerObjectRegistry;
	
	const UART_HandleTypeDef* huart = &UART_Handle_Uart2_Default;
	const GPIOEX_TypeDef* rxpin = &PD6_As_Uart2Rx_Default;
	const GPIOEX_TypeDef* txpin = &PD5_As_Uart2Tx_Default;
	const DMA_HandleTypeDef* hdmarx = &DMA_Handle_Uart2Rx_Default;
	const IRQ_HandleTypeDef* hirq_dmarx = &IRQ_Handle_Uart2RxDMA_Default;
	const DMA_HandleTypeDef* hdmatx = &DMA_Handle_Uart2Tx_Default;
	const IRQ_HandleTypeDef* hirq_dmatx = &IRQ_Handle_Uart2TxDMA_Default;
	const IRQ_HandleTypeDef* hirq_uart = &IRQ_Handle_Uart2_Default;
	
	h = UARTEX_Handle_FactoryCreate(&factory, huart, rxpin, txpin, hdmarx, hirq_dmarx, hdmatx, hirq_dmatx, hirq_uart);
	UARTEX_Handle_FactoryDestroy(&factory, h);
}

TEST_GROUP_RUNNER(UARTEX_Handle)
{
	RUN_TEST_CASE(UARTEX_Handle, Ctor);
	RUN_TEST_CASE(UARTEX_Handle, Dtor);
	RUN_TEST_CASE(UARTEX_Handle, FactoryCreate);
	RUN_TEST_CASE(UARTEX_Handle, FactoryDestroy);
}

/////////////////////////////////////
// TEST GROUP BEGIN - UARTEX_Clone

//TEST_GROUP(UARTEX_Clone);
//TEST_SETUP(UARTEX_Clone)
//{
//}

//TEST_TEAR_DOWN(UARTEX_Clone)
//{
//}

//TEST(UARTEX_Clone, CloneNoIRQNoDMA)
//{
//	UARTEX_HandleTypeDef dst;

//	int 	size = sizeof(UART_HandleTypeDef);
//	
//	UARTEX_Clone(&dst, 	&UARTEX_Handle_Uart2_Default, &PD6_As_Uart2Rx_Default, 
//		&PD5_As_Uart2Tx_Default, NULL, NULL, NULL);	
//	
//	TEST_ASSERT_EQUAL_MEMORY(&PD6_As_Uart2Rx_Default, &dst.rxpin, sizeof(GPIOEX_TypeDef));
//	TEST_ASSERT_EQUAL_MEMORY(&PD5_As_Uart2Tx_Default, &dst.txpin, sizeof(GPIOEX_TypeDef));
//	TEST_ASSERT_EQUAL_MEMORY(&UARTEX_Handle_Uart2_Default.huart, &dst.huart, size);
//	TEST_ASSERT_FALSE(dst.useIRQ);
//}

//TEST(UARTEX_Clone, CloneIRQNoDMA)
//{
//	UARTEX_HandleTypeDef dst;
//	
//	UARTEX_Clone(&dst, 	&UARTEX_Handle_Uart2_Default, &PD6_As_Uart2Rx_Default, 
//		&PD5_As_Uart2Tx_Default, &IRQ_Handle_Uart2_Default, NULL, NULL);	
//	
//	TEST_ASSERT_TRUE(dst.useIRQ);
////	TEST_ASSERT_EQUAL(&dst.huart, dst.hirq.hdata);
////	dst.hirq.hdata = 0;
//	TEST_ASSERT_EQUAL_MEMORY(&IRQ_Handle_Uart2_Default, &dst.hirq, sizeof(IRQ_HandleTypeDef));
//}

//TEST(UARTEX_Clone, CloneIRQDMA)
//{
//	UARTEX_HandleTypeDef dst;
//	DMAEX_HandleTypeDef rxdma;
//	DMAEX_HandleTypeDef txdma;
//	
//	rxdma = DMAEX_Handle_Uart2Rx_Default;
//	txdma = DMAEX_Handle_Uart2Rx_Default;
//	
//	UARTEX_Clone(&dst, 	&UARTEX_Handle_Uart2_Default, &PD6_As_Uart2Rx_Default, 
//		&PD5_As_Uart2Tx_Default, &IRQ_Handle_Uart2_Default, &rxdma, &txdma);
//	
//	TEST_ASSERT_EQUAL_HEX32(&rxdma, dst.hdmaex_rx);
//	TEST_ASSERT_EQUAL_HEX32(&txdma, dst.hdmaex_tx);
//	
////	TEST_ASSERT_EQUAL_HEX32(&rxdma.hdma, dst.huart.hdmarx);
////	TEST_ASSERT_EQUAL_HEX32(&txdma.hdma, dst.huart.hdmatx);
////	
////	TEST_ASSERT_EQUAL_HEX32(&dst.huart, rxdma.hdma.Parent);
////	TEST_ASSERT_EQUAL_HEX32(&dst.huart, txdma.hdma.Parent);
//}

//TEST_GROUP_RUNNER(UARTEX_Clone)
//{
//	RUN_TEST_CASE(UARTEX_Clone, CloneNoIRQNoDMA);
//	RUN_TEST_CASE(UARTEX_Clone, CloneIRQNoDMA);
//	RUN_TEST_CASE(UARTEX_Clone, CloneIRQDMA);
//}

TEST_GROUP(UsartHalEx_DMA);
TEST_SETUP(UsartHalEx_DMA)
{	
	refresh_static_variables();
	HAL_UART_Init(HUART_DMA);
}

TEST_TEAR_DOWN(UsartHalEx_DMA)
{
	volatile uint32_t dr;

	HAL_UART_DMAStop(HUART_DMA);
	
	/** read to clear ORE **/
	if (HUART_DMA->Instance->SR & USART_SR_RXNE)
		dr = HUART_DMA->Instance->DR;
		
	HAL_UART_DeInit(HUART_DMA);
}

TEST(UsartHalEx_DMA, SwapRxDMABufferInvalidArgs)
{
//	HAL_StatusTypeDef HAL_UART_SwapRxDMABuffer(UART_HandleTypeDef* h, uint8_t* buf, size_t size, uint32_t* m0ar, int* ndtr)

	TEST_ASSERT_EQUAL(HAL_ERROR, HAL_UART_SwapRxDMABuffer(0, (uint8_t*)1, 1, 0, 0));			/** null handle **/
	TEST_ASSERT_EQUAL(HAL_ERROR, HAL_UART_SwapRxDMABuffer(HUART_DMA, 0, 1, 0, 0));						/** null buf pointer **/
	TEST_ASSERT_EQUAL(HAL_ERROR, HAL_UART_SwapRxDMABuffer(HUART_DMA, (uint8_t*)1, 0, 0, 0));	/** zero pointer size **/
}

TEST(UsartHalEx_DMA, SwapRxDMABufferInvalidUartHalState)
{
	UART_HandleTypeDef handle;
	char chr;
	
	memset(&handle, 0, sizeof(handle));
	
	/** treat RESET as TIMEOUT since it can not proceed either, see below **/
	handle.State = HAL_UART_STATE_RESET;
	TEST_ASSERT_EQUAL(HAL_TIMEOUT, HAL_UART_SwapRxDMABuffer(&handle, (uint8_t*)&chr, 1, 0, 0));
	
	/** 
	For Firmware 1.1.0, UART HAL do *NOT* really use this state. 
	
	The error policy seems to be:
	(1) If the error does NOT prevent hardware from working, the state 
			is READY and ErrorCode is set. The read / write operation can proceed.
	(2) The only error that can not proceed is TIMEOUT, which occurs when set/clear
			register bit and no response. 
	
	So here we treat HAL_UART_STATE_ERROR as non-recoverable error, just as TIMEOUT
	**/
	handle.State = HAL_UART_STATE_ERROR;
	TEST_ASSERT_EQUAL(HAL_TIMEOUT, HAL_UART_SwapRxDMABuffer(&handle, (uint8_t*)&chr, 1, 0, 0));
	
	handle.State = HAL_UART_STATE_BUSY;
	TEST_ASSERT_EQUAL(HAL_BUSY, HAL_UART_SwapRxDMABuffer(&handle, (uint8_t*)&chr, 1, 0, 0));
	
	handle.State = HAL_UART_STATE_TIMEOUT;
	TEST_ASSERT_EQUAL(HAL_TIMEOUT, HAL_UART_SwapRxDMABuffer(&handle, (uint8_t*)&chr, 1, 0, 0));
}


TEST(UsartHalEx_DMA, SwapRxDMABufferReplaceEmptyBuffer)
{
	char original[16];
	char replace[16];
	uint32_t m0addr;
	int count;
	HAL_StatusTypeDef status;
	
	/** prepare **/
	TEST_ASSERT_EQUAL(HAL_UART_STATE_READY, HUART_DMA->State);
	TEST_ASSERT_EQUAL(HAL_OK, HAL_UART_Receive_DMA(HUART_DMA, (uint8_t*)original, sizeof(original)));
	TEST_ASSERT_EQUAL_HEX8(HAL_UART_STATE_BUSY_RX, HUART_DMA->State);
	
	/** do **/
	status  = HAL_UART_SwapRxDMABuffer(HUART_DMA, (uint8_t*)replace, sizeof(replace), &m0addr, &count);
	
	/** check **/
	TEST_ASSERT_EQUAL(HAL_OK, status);
	TEST_ASSERT_EQUAL(HAL_UART_STATE_BUSY_RX, HUART_DMA->State);
	TEST_ASSERT_EQUAL_HEX32((uint32_t)original, m0addr);
	TEST_ASSERT_EQUAL(sizeof(original), count);
	
	/** verify registers **/
	TEST_ASSERT_EQUAL_HEX32((uint32_t)replace, HUART_DMA->hdmarx->Instance->M0AR);
	TEST_ASSERT_EQUAL_HEX32((uint32_t)sizeof(replace), __HAL_DMA_GET_COUNTER(HUART_DMA->hdmarx));
}

TEST_GROUP_RUNNER(UsartHalEx_DMA)
{
	RUN_TEST_CASE(UsartHalEx_DMA, SwapRxDMABufferInvalidArgs);
	RUN_TEST_CASE(UsartHalEx_DMA, SwapRxDMABufferInvalidUartHalState);
	RUN_TEST_CASE(UsartHalEx_DMA, SwapRxDMABufferReplaceEmptyBuffer);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////






















// TEST GROUP END - UARTEX_Clone   //










