/************************************************************************/
/* includes                                                             */
/************************************************************************/

#include <asf.h>
#include <string.h>
#include "arm_math.h"
#include "ili9341.h"
#include "lvgl.h"
#include "touch/touch.h"
#include "aps2/aps2.h"
#include "aps2/ecg.h"

#include "ili9341.h"

#include "Image.h"
#include "miniLogo.h"

LV_FONT_DECLARE(dseg30);
LV_FONT_DECLARE(dseg50);
LV_FONT_DECLARE(dseg70);

/** Display background color when clearing the display */
#define BG_COLOR  ILI9341_COLOR(255, 255, 255)

#define AFEC_POT AFEC1
#define AFEC_POT_ID ID_AFEC1
#define AFEC_POT_CHANNEL 6
/************************************************************************/
/* STATIC                                                               */
/************************************************************************/

/*A static or global variable to store the buffers*/
static lv_disp_buf_t disp_buf;

/*Static or global buffer(s). The second buffer is optional*/
static lv_color_t buf_1[LV_HOR_RES_MAX * LV_VER_RES_MAX];

static  lv_obj_t * labelStart;
static  lv_obj_t * labelPower;
static lv_obj_t * spinbox;
static lv_obj_t * spinbox2;
static lv_obj_t * labelBat;
static lv_obj_t * labelTempo;
static lv_obj_t * labelOx;
static lv_obj_t * labelOxUni;
static lv_obj_t * labelOxNum;
static lv_obj_t * labelBa;
static lv_obj_t * labelBaUni;
static lv_obj_t * labelBaNum;


volatile bool g_is_conversion_done = false;
/** The conversion data value */
volatile uint32_t g_ul_value = 0;


typedef struct {
	uint value;
} ecgData;


QueueHandle_t xQueueECG;
/************************************************************************/
/* RTOS                                                                 */
/************************************************************************/

#define TASK_LCD_STACK_SIZE          (1024*6/sizeof(portSTACK_TYPE))
#define TASK_LCD_PRIORITY            (tskIDLE_PRIORITY)

#define TASK_APS2_STACK_SIZE         (1024*6/sizeof(portSTACK_TYPE))
#define TASK_APS2_PRIORITY           (tskIDLE_PRIORITY)

#define TASK_MAIN_STACK_SIZE         (1024*6/sizeof(portSTACK_TYPE))
#define TASK_MAIN_PRIORITY           (tskIDLE_PRIORITY)

extern void vApplicationStackOverflowHook(xTaskHandle *pxTask, signed char *pcTaskName) {
  printf("stack overflow %x %s\r\n", pxTask, (portCHAR *)pcTaskName);
  for (;;) {	}
}

extern void vApplicationIdleHook(void) { }

extern void vApplicationTickHook(void) { }

extern void vApplicationMallocFailedHook(void) {  configASSERT( ( volatile void * ) NULL ); }
	
	
	
/************************************************************************/
/* AFEC                                                                 */
/************************************************************************/

static void AFEC_pot_Callback(void){
	g_ul_value = afec_channel_get_value(AFEC_POT, AFEC_POT_CHANNEL);
	g_is_conversion_done = true;
	ecgData ecg;
	ecg.value=g_ul_value;
	xQueueSendFromISR(xQueueECG,&ecg,0);
}

static void config_AFEC_pot(Afec *afec, uint32_t afec_id, uint32_t afec_channel, afec_callback_t callback){
	/*************************************
	* Ativa e configura AFEC
	*************************************/
	/* Ativa AFEC - 0 */
	afec_enable(afec);

	/* struct de configuracao do AFEC */
	struct afec_config afec_cfg;

	/* Carrega parametros padrao */
	afec_get_config_defaults(&afec_cfg);

	/* Configura AFEC */
	afec_init(afec, &afec_cfg);

	/* Configura trigger por software */
	afec_set_trigger(afec, AFEC_TRIG_SW);

	/*** Configuracao espec�fica do canal AFEC ***/
	struct afec_ch_config afec_ch_cfg;
	afec_ch_get_config_defaults(&afec_ch_cfg);
	afec_ch_cfg.gain = AFEC_GAINVALUE_0;
	afec_ch_set_config(afec, afec_channel, &afec_ch_cfg);

	/*
	* Calibracao:
	* Because the internal ADC offset is 0x200, it should cancel it and shift
	down to 0.
	*/
	afec_channel_set_analog_offset(afec, afec_channel, 0x200);

	/***  Configura sensor de temperatura ***/
	struct afec_temp_sensor_config afec_temp_sensor_cfg;

	afec_temp_sensor_get_config_defaults(&afec_temp_sensor_cfg);
	afec_temp_sensor_set_config(afec, &afec_temp_sensor_cfg);
	
	/* configura IRQ */
	afec_set_callback(afec, afec_channel,	callback, 1);
	NVIC_SetPriority(afec_id, 4);
	NVIC_EnableIRQ(afec_id);
}
	
	
/************************************************************************/
/* TC                                                                   */
/************************************************************************/

void TC3_Handler(void){
	volatile uint32_t ul_dummy;

	/****************************************************************
	* Devemos indicar ao TC que a interrup��o foi satisfeita.
	******************************************************************/
	ul_dummy = tc_get_status(TC1, 0);

	/* Avoid compiler warning */
	UNUSED(ul_dummy);
	
	afec_channel_enable(AFEC_POT, AFEC_POT_CHANNEL);
	afec_start_software_conversion(AFEC_POT);

	/** Muda o estado do LED */
	
}

void TC_init(Tc * TC, int ID_TC, int TC_CHANNEL, int freq){
	uint32_t ul_div;
	uint32_t ul_tcclks;
	uint32_t ul_sysclk = sysclk_get_cpu_hz();

	/* Configura o PMC */
	/* O TimerCounter � meio confuso
	o uC possui 3 TCs, cada TC possui 3 canais
	TC0 : ID_TC0, ID_TC1, ID_TC2
	TC1 : ID_TC3, ID_TC4, ID_TC5
	TC2 : ID_TC6, ID_TC7, ID_TC8
	*/
	pmc_enable_periph_clk(ID_TC);

	/** Configura o TC para operar em  freq hz e interrup�c�o no RC compare */
	tc_find_mck_divisor(freq, ul_sysclk, &ul_div, &ul_tcclks, ul_sysclk);
	tc_init(TC, TC_CHANNEL, ul_tcclks | TC_CMR_CPCTRG);
	tc_write_rc(TC, TC_CHANNEL, (ul_sysclk / ul_div) / freq);

	/* Configura e ativa interrup�c�o no TC canal 0 */
	/* Interrup��o no C */
	NVIC_SetPriority(ID_TC, 4);
	NVIC_EnableIRQ((IRQn_Type) ID_TC);
	tc_enable_interrupt(TC, TC_CHANNEL, TC_IER_CPCS);

	/* Inicializa o canal 0 do TC */
	tc_start(TC, TC_CHANNEL);
}
/************************************************************************/
/* HANDLERS E EVENTS                                                               */
/************************************************************************/

static void event_handler(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {
		printf("Clicked\n");
	}
	else if(event == LV_EVENT_VALUE_CHANGED) {
		printf("Toggled\n");
	}
}

static void event_handler2(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {
		printf("Clicked\n");
	}
	else if(event == LV_EVENT_VALUE_CHANGED) {
		printf("Toggled\n");
	}
}

static void lv_spinbox_increment_event_cb(lv_obj_t * btn, lv_event_t e)
{
	if(e == LV_EVENT_SHORT_CLICKED || e == LV_EVENT_LONG_PRESSED_REPEAT) {
		lv_spinbox_increment(spinbox);
	}
}

static void lv_spinbox_decrement_event_cb(lv_obj_t * btn, lv_event_t e)
{
	if(e == LV_EVENT_SHORT_CLICKED || e == LV_EVENT_LONG_PRESSED_REPEAT) {
		lv_spinbox_decrement(spinbox);
	}
}

static void lv_spinbox_increment_event_cb2(lv_obj_t * btn, lv_event_t e)
{
	if(e == LV_EVENT_SHORT_CLICKED || e == LV_EVENT_LONG_PRESSED_REPEAT) {
		lv_spinbox_increment(spinbox2);
	}
}

static void lv_spinbox_decrement_event_cb2(lv_obj_t * btn, lv_event_t e)
{
	if(e == LV_EVENT_SHORT_CLICKED || e == LV_EVENT_LONG_PRESSED_REPEAT) {
		lv_spinbox_decrement(spinbox2);
	}
}

/************************************************************************/
/* ASP2 - NAO MEXER!                                                    */
/************************************************************************/

QueueHandle_t xQueueOx;
TimerHandle_t xTimer;
volatile int g_ecgCnt = 0;
volatile int g_ecgDelayCnt = 0;
volatile int g_ecgDelayValue = 0;
const int g_ecgSize =  sizeof(ecg)/sizeof(ecg[0]);

/************************************************************************/
/* lvgl                                                                 */
/************************************************************************/
void lv_inicio(void){
	lv_obj_set_style_local_bg_color(lv_scr_act(), LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_WHITE);
	
	//-------------------
	// LOGO
	//-------------------

	lv_obj_t * img1 = lv_img_create(lv_scr_act(), NULL);
	lv_img_set_src(img1, &Image);
	lv_obj_align(img1, NULL, LV_ALIGN_CENTER, 0, -80);
	
	//-------------------
	// INICIAR
	//-------------------

	// cria botao de tamanho 60x60 redondo
	lv_obj_t * btnStart = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btnStart, event_handler);
	lv_obj_set_width(btnStart, 100);  lv_obj_set_height(btnStart, 30);

	// alinha no canto esquerdo e desloca um pouco para cima e para direita
	lv_obj_align(btnStart, NULL, LV_ALIGN_IN_BOTTOM_MID, 0, -27);

	// altera a cor de fundo, borda do bot�o criado para PRETO
	lv_obj_set_style_local_bg_color(btnStart, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAGENTA);
	lv_obj_set_style_local_border_color(btnStart, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT,  LV_COLOR_MAGENTA);
	lv_obj_set_style_local_border_width(btnStart, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
	
	labelStart = lv_label_create(btnStart, NULL);
	lv_label_set_recolor(labelStart, true);
	lv_label_set_text(labelStart, "#2D9613 INICIAR ");
	
	//-------------------
	// SPINBOX ESQUERDA
	//-------------------
	
	spinbox = lv_spinbox_create(lv_scr_act(), NULL);
	lv_spinbox_set_range(spinbox, 0, 23);
	lv_spinbox_set_digit_format(spinbox, 2, 2);
	lv_spinbox_step_prev(spinbox);
	lv_obj_set_width(spinbox, 40);
	lv_obj_align(spinbox, NULL, LV_ALIGN_CENTER, -70, 0);

	lv_coord_t h = lv_obj_get_height(spinbox);
	lv_obj_t * btn = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_size(btn, h, h);
	lv_obj_align(btn, spinbox, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
	lv_theme_apply(btn, LV_THEME_SPINBOX_BTN);
	lv_obj_set_style_local_value_str(btn, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, LV_SYMBOL_PLUS);
	lv_obj_set_event_cb(btn, lv_spinbox_increment_event_cb);

	btn = lv_btn_create(lv_scr_act(), btn);
	lv_obj_align(btn, spinbox, LV_ALIGN_OUT_LEFT_MID, -5, 0);
	lv_obj_set_event_cb(btn, lv_spinbox_decrement_event_cb);
	lv_obj_set_style_local_value_str(btn, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, LV_SYMBOL_MINUS);
	
	//-------------------
	// SPINBOX DIREITA
	//-------------------
	
	spinbox2 = lv_spinbox_create(lv_scr_act(), NULL);
	lv_spinbox_set_range(spinbox2, 0, 59);
	lv_spinbox_set_digit_format(spinbox2, 2, 2);
	lv_spinbox_step_prev(spinbox2);
	lv_obj_set_width(spinbox2, 40);
	lv_obj_align(spinbox2, NULL, LV_ALIGN_CENTER,70, 0);

	lv_coord_t h2 = lv_obj_get_height(spinbox2);
	lv_obj_t * btn2 = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_size(btn2, h2, h2);
	lv_obj_align(btn2, spinbox2, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
	lv_theme_apply(btn2, LV_THEME_SPINBOX_BTN);
	lv_obj_set_style_local_value_str(btn2, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, LV_SYMBOL_PLUS);
	lv_obj_set_event_cb(btn2, lv_spinbox_increment_event_cb2);

	btn2 = lv_btn_create(lv_scr_act(), btn2);
	lv_obj_align(btn2, spinbox2, LV_ALIGN_OUT_LEFT_MID, -5, 0);
	lv_obj_set_event_cb(btn2, lv_spinbox_decrement_event_cb2);
	lv_obj_set_style_local_value_str(btn2, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, LV_SYMBOL_MINUS);
	
	//-------------------
	// HORA
	//-------------------
	
	lv_obj_t * label1 = lv_label_create(lv_scr_act(), NULL);
	lv_label_set_long_mode(label1, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(label1, true);
	lv_obj_align(label1, NULL, LV_ALIGN_CENTER, -76, -35);
	lv_label_set_text(label1, "#000000 HORA");
	lv_obj_set_width(label1, 150);
	
	
	//-------------------
	//minuto
	//-------------------
	
	lv_obj_t * label2 = lv_label_create(lv_scr_act(), NULL);
	lv_label_set_long_mode(label2, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(label2, true);
	lv_obj_align(label2, NULL, LV_ALIGN_CENTER, 55, -35);
	lv_label_set_text(label2, "#000000 MINUTO");
	lv_obj_set_width(label2, 150);
}

void lv_principal(void){
	lv_obj_set_style_local_bg_color(lv_scr_act(), LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_WHITE);
	
	//-------------------
	// MINI LOGO
	//-------------------

	lv_obj_t * img2 = lv_img_create(lv_scr_act(), NULL);
	lv_img_set_src(img2, &miniLogo);
	lv_obj_align(img2, NULL, LV_ALIGN_IN_TOP_RIGHT, -10, 5);
	
	//-------------------
	// Desligar
	//-------------------

	// cria botao de tamanho 60x60 redondo
	lv_obj_t * btnPower = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btnPower, event_handler);
	lv_obj_set_width(btnPower, 40);  lv_obj_set_height(btnPower, 40);

	// alinha no canto esquerdo e desloca um pouco para cima e para direita
	lv_obj_align(btnPower, NULL, LV_ALIGN_IN_BOTTOM_RIGHT, -10, -10);

	// altera a cor de fundo, borda do bot�o criado para PRETO
	lv_obj_set_style_local_bg_color(btnPower, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAGENTA );
	lv_obj_set_style_local_border_color(btnPower, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAGENTA );
	lv_obj_set_style_local_border_width(btnPower, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
	
	labelPower = lv_label_create(btnPower, NULL);
	lv_label_set_recolor(labelPower, true);
	lv_label_set_text(labelPower, "#ffffff  " LV_SYMBOL_POWER " ");
	
	//-------------------
	// Bateria
	//-------------------

	lv_obj_t * labelBat = lv_label_create(lv_scr_act(), NULL);
	lv_label_set_long_mode(labelBat, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(labelBat, true);
	lv_obj_align(labelBat, NULL, LV_ALIGN_IN_TOP_RIGHT, -80, 25);
	lv_label_set_text(labelBat, "#2D9613  " LV_SYMBOL_BATTERY_3 " 76% ");
	lv_obj_set_width(labelBat, 150);
	
	//-------------------
	//Hora e minuto
	//-------------------
	
	lv_obj_t * labelTempo = lv_label_create(lv_scr_act(), NULL);
	lv_label_set_long_mode(labelTempo, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(labelTempo, true);
	lv_obj_align(labelTempo, NULL, LV_ALIGN_IN_TOP_MID, 0, 25);
	lv_label_set_text(labelTempo, "#000000 17:04");
	lv_obj_set_width(labelTempo, 150);
	
	//-------------------
	//Oxigenio
	//-------------------
	
	lv_obj_t * labelOx = lv_label_create(lv_scr_act(), NULL);
	lv_label_set_long_mode(labelOx, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(labelOx, true);
	lv_obj_align(labelOx, NULL, LV_ALIGN_IN_LEFT_MID, 40, -50);
	lv_label_set_text(labelOx, "#000000 OXIGENIO");
	lv_obj_set_width(labelOx, 150);
	
	
	lv_obj_t * labelOxNum = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(labelOxNum, NULL, LV_ALIGN_IN_LEFT_MID, 40 , -28);
	lv_obj_set_style_local_text_font(labelOxNum, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &dseg30);
	lv_obj_set_style_local_text_color(labelOxNum, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAGENTA);
	//lv_label_set_text_fmt(labelOxNum, "97");
	
	lv_obj_t * labelOxUni = lv_label_create(lv_scr_act(), NULL);
	lv_label_set_long_mode(labelOxUni, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(labelOxUni, true);
	lv_obj_align(labelOxUni, NULL, LV_ALIGN_IN_LEFT_MID, 97, -13);
	lv_label_set_text(labelOxUni, "#CA1041 SpO2%");
	lv_obj_set_width(labelOxUni, 150);
	
	//-------------------
	// Batimentos
	//-------------------
	
	lv_obj_t * labelBa = lv_label_create(lv_scr_act(), NULL);
	lv_label_set_long_mode(labelBa, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(labelBa, true);
	lv_obj_align(labelBa, NULL, LV_ALIGN_IN_LEFT_MID, 40, 20);
	lv_label_set_text(labelBa, "#000000 BATIMENTOS");
	lv_obj_set_width(labelBa, 150);
	
	
	lv_obj_t * labelBaNum = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(labelBaNum, NULL, LV_ALIGN_IN_LEFT_MID, 28 , 42);
	lv_obj_set_style_local_text_font(labelBaNum, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &dseg30);
	lv_obj_set_style_local_text_color(labelBaNum, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_GREEN);
	lv_label_set_text_fmt(labelBaNum, "170");
	
	lv_obj_t *labelBaUni = lv_label_create(lv_scr_act(), NULL);
	lv_label_set_long_mode(labelBaUni, LV_LABEL_LONG_BREAK);
	lv_label_set_recolor(labelBaUni, true);
	lv_obj_align(labelBaUni, NULL, LV_ALIGN_IN_LEFT_MID, 105, 60);
	lv_label_set_text(labelBaUni, "#2D9613 BPM");
	lv_obj_set_width(labelBaUni, 150);
	
	
}

/************************************************************************/
/* TASKS                                                                */
/************************************************************************/

static void task_lcd(void *pvParameters) {
	
	LV_IMG_DECLARE(Image);
	LV_IMG_DECLARE(miniLogo);
	//lv_inicio();
	lv_principal();
	
  for (;;)  {
    lv_tick_inc(50);
    lv_task_handler();
    vTaskDelay(50);
  }
}

static void task_main(void *pvParameters) {

   char ox;
   for (;;)  {
    
    if ( xQueueReceive( xQueueOx, &ox, 0 )) {
      printf("ox: %d \n", ox);
	  lv_label_set_text_fmt(labelOxNum, "%d",ox);
    }
	
	 printf("ox sem fila: %d \n", ox);
 
    vTaskDelay(25);
  }
}

static void task_process(void *pvParameters) {
	
	TC_init(TC1, ID_TC3, 0, 250);
	xQueueECG = xQueueCreate(250, sizeof(int));
	ecgData ecg;
	config_AFEC_pot(AFEC_POT, AFEC_POT_ID, AFEC_POT_CHANNEL, AFEC_pot_Callback);
	if (xQueueECG == NULL){
		printf("falha em criar a fila \n");
	}
	while(1){
		if (xQueueReceive( xQueueECG, &(ecg), ( TickType_t ) 10/ portTICK_PERIOD_MS)) {
			printf("%d \n", ecg.value);
		}
		
	}
}

/************************************************************************/
/* configs                                                              */
/************************************************************************/

static void configure_lcd(void) {
  /**LCD pin configure on SPI*/
  pio_configure_pin(LCD_SPI_MISO_PIO, LCD_SPI_MISO_FLAGS);  //
  pio_configure_pin(LCD_SPI_MOSI_PIO, LCD_SPI_MOSI_FLAGS);
  pio_configure_pin(LCD_SPI_SPCK_PIO, LCD_SPI_SPCK_FLAGS);
  pio_configure_pin(LCD_SPI_NPCS_PIO, LCD_SPI_NPCS_FLAGS);
  pio_configure_pin(LCD_SPI_RESET_PIO, LCD_SPI_RESET_FLAGS);
  pio_configure_pin(LCD_SPI_CDS_PIO, LCD_SPI_CDS_FLAGS);
  
}

static void configure_console(void) {
  const usart_serial_options_t uart_serial_options = {
    .baudrate = USART_SERIAL_EXAMPLE_BAUDRATE,
    .charlength = USART_SERIAL_CHAR_LENGTH,
    .paritytype = USART_SERIAL_PARITY,
    .stopbits = USART_SERIAL_STOP_BIT,
  };

  /* Configure console UART. */
  stdio_serial_init(CONSOLE_UART, &uart_serial_options);

  /* Specify that stdout should not be buffered. */
  setbuf(stdout, NULL);
}

/************************************************************************/
/* port lvgl                                                            */
/************************************************************************/

void my_flush_cb(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p) {
  ili9341_set_top_left_limit(area->x1, area->y1);   ili9341_set_bottom_right_limit(area->x2, area->y2);
  ili9341_copy_pixels_to_screen(color_p,  (area->x2 - area->x1) * (area->y2 - area->y1));
  
  /* IMPORTANT!!!
  * Inform the graphics library that you are ready with the flushing*/
  lv_disp_flush_ready(disp_drv);
}

bool my_input_read(lv_indev_drv_t * drv, lv_indev_data_t*data) {
  int px, py, pressed;
  
  if (readPoint(&px, &py)) {
    data->state = LV_INDEV_STATE_PR;
  }
  else {
    data->state = LV_INDEV_STATE_REL;
  }
  
  data->point.x = px;
  data->point.y = py;
  return false; /*No buffering now so no more data read*/
}

/************************************************************************/
/* main                                                                 */
/************************************************************************/
int main(void) {
  /* board and sys init */
  board_init();
  sysclk_init();
  configure_console();

  /* LCd int */
  configure_lcd();
  ili9341_init();
  configure_touch();
  ili9341_backlight_on();
  
 
  
  /*LittlevGL init*/
  lv_init();
  lv_disp_drv_t disp_drv;                 /*A variable to hold the drivers. Can be local variable*/
  lv_disp_drv_init(&disp_drv);            /*Basic initialization*/
  lv_disp_buf_init(&disp_buf, buf_1, NULL, LV_HOR_RES_MAX * LV_VER_RES_MAX);  /*Initialize `disp_buf` with the buffer(s) */
  disp_drv.buffer = &disp_buf;            /*Set an initialized buffer*/
  disp_drv.flush_cb = my_flush_cb;        /*Set a flush callback to draw to the display*/
  lv_disp_t * disp;
  disp = lv_disp_drv_register(&disp_drv); /*Register the driver and save the created display objects*/
  
  /* Init input on LVGL */
  lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);      /*Basic initialization*/
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_input_read;
  /*Register the driver in LVGL and save the created input device object*/
  lv_indev_t * my_indev = lv_indev_drv_register(&indev_drv);
  
  xQueueOx = xQueueCreate(32, sizeof(char));

  if (xTaskCreate(task_lcd, "LCD", TASK_LCD_STACK_SIZE, NULL, TASK_LCD_PRIORITY, NULL) != pdPASS) {
    printf("Failed to create lcd task\r\n");
  }
  
  if (xTaskCreate(task_aps2, "APS2", TASK_APS2_STACK_SIZE, NULL, TASK_APS2_PRIORITY, NULL) != pdPASS) {
    printf("Failed to create APS task\r\n");
  }
  
  if (xTaskCreate(task_main, "main", TASK_MAIN_STACK_SIZE, NULL, TASK_MAIN_PRIORITY, NULL) != pdPASS) {
    printf("Failed to create Main task\r\n");
  }
  

  if (xTaskCreate(task_process, "process", TASK_MAIN_STACK_SIZE, NULL, TASK_MAIN_PRIORITY, NULL) != pdPASS) {
	  printf("Failed to create process task\r\n");
  }
   
  /* Start the scheduler. */
  vTaskStartScheduler();



  /* RTOS n?o deve chegar aqui !! */
  while(1){ }
}
