/**
  ******************************************************************************
  * @file			console.c
  * @brief			console function
  * @author			Xli
  * @email			xieliyzh@163.com
  * @version		1.0.0
  * @date			2020-06-17
  * @copyright		2020, XIELI Co.,Ltd. All rights reserved
  ******************************************************************************
**/

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"
#include <rtthread.h>
#include <string.h>

/* Private constants ---------------------------------------------------------*/
#define CONSOLE_GET_CHAR_INT_MODE   /*!< 中断方式获取终端输入字符 */

#define UART_RX_BUF_LEN     16
#define USART_RX_Pin        GPIO_PIN_9
#define USART_TX_Pin        GPIO_PIN_10
/* Private macro -------------------------------------------------------------*/
#define rt_ringbuffer_space_len(rb) ((rb)->buffer_size - rt_ringbuffer_data_len(rb))

/* Private typedef -----------------------------------------------------------*/
struct rt_ringbuffer
{
    rt_uint8_t *buffer_ptr;

    rt_uint16_t read_mirror : 1;
    rt_uint16_t read_index : 15;
    rt_uint16_t write_mirror : 1;
    rt_uint16_t write_index : 15;

    rt_int16_t buffer_size;
};

enum rt_ringbuffer_state
{
    RT_RINGBUFFER_EMPTY,
    RT_RINGBUFFER_FULL,
    /* half full is neither full nor empty */
    RT_RINGBUFFER_HALFFULL,
};

/* Private variables ---------------------------------------------------------*/
rt_uint8_t uart_rx_buf[UART_RX_BUF_LEN] = {0};
struct rt_ringbuffer  uart_rxcb;         /* 定义一个 ringbuffer cb */
static UART_HandleTypeDef UartHandle;
static struct rt_semaphore shell_rx_sem; /* 定义一个静态信号量 */

/* Private function ----------------------------------------------------------*/

/**=============================================================================
 * @brief           ringbuffer状态
 *
 * @param[in]       none
 *
 * @return          none
 *============================================================================*/
rt_inline enum rt_ringbuffer_state rt_ringbuffer_status(struct rt_ringbuffer *rb)
{
    if (rb->read_index == rb->write_index)
    {
        if (rb->read_mirror == rb->write_mirror)
            return RT_RINGBUFFER_EMPTY;
        else
            return RT_RINGBUFFER_FULL;
    }
    return RT_RINGBUFFER_HALFFULL;
}

/**=============================================================================
 * @brief           get the size of data in rb
 *
 * @param[in]       none
 *
 * @return          none
 *============================================================================*/
rt_size_t rt_ringbuffer_data_len(struct rt_ringbuffer *rb)
{
    switch (rt_ringbuffer_status(rb))
    {
    case RT_RINGBUFFER_EMPTY:
        return 0;
    case RT_RINGBUFFER_FULL:
        return rb->buffer_size;
    case RT_RINGBUFFER_HALFFULL:
    default:
        if (rb->write_index > rb->read_index)
            return rb->write_index - rb->read_index;
        else
            return rb->buffer_size - (rb->read_index - rb->write_index);
    };
}

/**=============================================================================
 * @brief           ringbuffer init
 *
 * @param[in]       none
 *
 * @return          none
 *============================================================================*/
void rt_ringbuffer_init(struct rt_ringbuffer *rb,
                        rt_uint8_t           *pool,
                        rt_int16_t            size)
{
    RT_ASSERT(rb != RT_NULL);
    RT_ASSERT(size > 0);

    /* initialize read and write index */
    rb->read_mirror = rb->read_index = 0;
    rb->write_mirror = rb->write_index = 0;

    /* set buffer pool and size */
    rb->buffer_ptr = pool;
    rb->buffer_size = RT_ALIGN_DOWN(size, RT_ALIGN_SIZE);
}

/**=============================================================================
 * @brief           put a character into ring buffer
 *
 * @param[in]       none
 *
 * @return          none
 *============================================================================*/
rt_size_t rt_ringbuffer_putchar(struct rt_ringbuffer *rb, const rt_uint8_t ch)
{
    RT_ASSERT(rb != RT_NULL);

    /* whether has enough space */
    if (!rt_ringbuffer_space_len(rb))
        return 0;

    rb->buffer_ptr[rb->write_index] = ch;

    /* flip mirror */
    if (rb->write_index == rb->buffer_size-1)
    {
        rb->write_mirror = ~rb->write_mirror;
        rb->write_index = 0;
    }
    else
    {
        rb->write_index++;
    }

    return 1;
}

/**=============================================================================
 * @brief           get a character from a ringbuffer
 *
 * @param[in]       none
 *
 * @return          none
 *============================================================================*/
rt_size_t rt_ringbuffer_getchar(struct rt_ringbuffer *rb, rt_uint8_t *ch)
{
    RT_ASSERT(rb != RT_NULL);

    /* ringbuffer is empty */
    if (!rt_ringbuffer_data_len(rb))
        return 0;

    /* put character */
    *ch = rb->buffer_ptr[rb->read_index];

    if (rb->read_index == rb->buffer_size-1)
    {
        rb->read_mirror = ~rb->read_mirror;
        rb->read_index = 0;
    }
    else
    {
        rb->read_index++;
    }

    return 1;
}

/**=============================================================================
 * @brief           初始化串口，中断方式
 *
 * @param[in]       none
 *
 * @return          none
 *============================================================================*/
static int rt_hw_uart_init(void)
{
    /* 初始化串口接收 ringbuffer  */
    rt_ringbuffer_init(&uart_rxcb, uart_rx_buf, UART_RX_BUF_LEN);
#ifdef CONSOLE_GET_CHAR_INT_MODE 
    /* 初始化串口接收数据的信号量 */
    rt_sem_init(&(shell_rx_sem), "shell_rx", 0, 0); /*!< @TODO 使能后程序异常 */
#endif
    /* 初始化串口参数，如波特率、停止位等等 */
    UartHandle.Instance = USART1;
    UartHandle.Init.BaudRate   = 115200;
    UartHandle.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
    UartHandle.Init.Mode       = UART_MODE_TX_RX;
    UartHandle.Init.OverSampling = UART_OVERSAMPLING_16;
    UartHandle.Init.WordLength = UART_WORDLENGTH_8B;
    UartHandle.Init.StopBits   = UART_STOPBITS_1;
    UartHandle.Init.Parity     = UART_PARITY_NONE;

    /* 初始化串口引脚等 */
    if (HAL_UART_Init(&UartHandle) != HAL_OK)
    {
        while (1);
    }

#ifdef CONSOLE_GET_CHAR_INT_MODE
    /* 中断配置 */
    __HAL_UART_ENABLE_IT(&UartHandle, UART_IT_RXNE);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    HAL_NVIC_SetPriority(USART1_IRQn, 3, 3);
#endif 

    return 0;
}
INIT_BOARD_EXPORT(rt_hw_uart_init);

/**=============================================================================
 * @brief           rt_hw_console_output
 *
 * @param[in]       none
 *
 * @return          none
 * 
 * @note            移植控制台，实现控制台输出, 对接 rt_hw_console_output
 *============================================================================*/
void rt_hw_console_output(const char *str)
{
    rt_size_t i = 0, size = 0;
    char a = '\r';

    __HAL_UNLOCK(&UartHandle);

    size = rt_strlen(str);
    for (i = 0; i < size; i++)
    {
        if (*(str + i) == '\n')
        {
            HAL_UART_Transmit(&UartHandle, (uint8_t *)&a, 1, 1);
        }
        HAL_UART_Transmit(&UartHandle, (uint8_t *)(str + i), 1, 1);
    }
}

/**=============================================================================
 * @brief           rt_hw_console_getchar
 *
 * @param[in]       none
 *
 * @return          none
 * 
 * @note            移植 FinSH，实现命令行交互, 需要添加 FinSH 源码，
 *                  然后再对接 rt_hw_console_getchar
 *============================================================================*/
#ifdef CONSOLE_GET_CHAR_INT_MODE
char rt_hw_console_getchar(void)
{
    char ch = 0;

    /* 从 ringbuffer 中拿出数据 */
    while (rt_ringbuffer_getchar(&uart_rxcb, (rt_uint8_t *)&ch) != 1)
    {
        rt_sem_take(&shell_rx_sem, RT_WAITING_FOREVER);
    } 
    return ch;   
}
#else
char rt_hw_console_getchar(void)
{
    int ch = -1;

    if (__HAL_UART_GET_FLAG(&UartHandle, UART_FLAG_RXNE) != RESET)
    {
        ch = UartHandle.Instance->DR & 0xff;
    }
    else
    {
        if(__HAL_UART_GET_FLAG(&UartHandle, UART_FLAG_ORE) != RESET)
        {
            __HAL_UART_CLEAR_OREFLAG(&UartHandle);
        }
        rt_thread_mdelay(10);
    }
    return ch;
}
#endif

/**=============================================================================
 * @brief           串口中断
 *
 * @param[in]       none
 *
 * @return          none
 *============================================================================*/
#ifdef CONSOLE_GET_CHAR_INT_MODE
void USART1_IRQHandler(void)
{
    int ch = -1;
    rt_base_t level;
    /* enter interrupt */
    rt_interrupt_enter();          //在中断中一定要调用这对函数，进入中断

    if ((__HAL_UART_GET_FLAG(&(UartHandle), UART_FLAG_RXNE) != RESET) &&
        (__HAL_UART_GET_IT_SOURCE(&(UartHandle), UART_IT_RXNE) != RESET))
    {
        while (1)
        {
            ch = -1;
            if (__HAL_UART_GET_FLAG(&(UartHandle), UART_FLAG_RXNE) != RESET)
            {
                ch =  UartHandle.Instance->DR & 0xff;
            }
            if (ch == -1)
            {
                break;
            }  
            /* 读取到数据，将数据存入 ringbuffer */
            rt_ringbuffer_putchar(&uart_rxcb, ch);
        }        
        rt_sem_release(&shell_rx_sem);
    }

    /* leave interrupt */
    rt_interrupt_leave();    //在中断中一定要调用这对函数，离开中断
}
#endif 

/**=============================================================================
 * @brief           
 *
 * @param[in]       none
 *
 * @return          none
 *============================================================================*/
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    if (huart->Instance == USART1)
    {
		__HAL_RCC_GPIOA_CLK_ENABLE();			
		__HAL_RCC_USART1_CLK_ENABLE();			
		__HAL_RCC_AFIO_CLK_ENABLE();
	
		GPIO_InitStruct.Pin = GPIO_PIN_9;			//PA9
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;		
		GPIO_InitStruct.Pull = GPIO_PULLUP;			
		GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);	   	

		GPIO_InitStruct.Pin = GPIO_PIN_10;			//PA10
		GPIO_InitStruct.Mode = GPIO_MODE_AF_INPUT;	
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);	   		
    }
}

/**=============================================================================
 * @brief           终端硬件初始化
 *
 * @param[in]       none
 *
 * @return          none
 *============================================================================*/
void rt_console_init(void)
{
    rt_hw_uart_init();
}
