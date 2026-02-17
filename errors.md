/home/kty/work/threadx-esp32c6-project/components/threadx/port/tx_timer_interrupt.c: In function '_tx_port_setup_timer_interrupt':
/home/kty/work/threadx-esp32c6-project/components/threadx/port/tx_timer_interrupt.c:113:15: error: 'SYSTIMER_TARGET0_PERIOD_REG' undeclared (first use in this function); did you mean 'SYSTIMER_TARGET0_PERIOD_S'?
  113 |     write_reg(SYSTIMER_TARGET0_PERIOD_REG, TIMER_ALARM_PERIOD & 0x03FFFFFF);
      |               ^~~~~~~~~~~~~~~~~~~~~~~~~~~
      |               SYSTIMER_TARGET0_PERIOD_S
/home/kty/work/threadx-esp32c6-project/components/threadx/port/tx_timer_interrupt.c:113:15: note: each undeclared identifier is reported only once for each function it appears in
/home/kty/work/threadx-esp32c6-project/components/threadx/port/tx_timer_interrupt.c: In function '_tx_esp32c6_timer_isr':
/home/kty/work/threadx-esp32c6-project/components/threadx/port/tx_timer_interrupt.c:167:5: error: implicit declaration of function '_tx_timer_interrupt'; did you mean '_tx_timer_info_get'? [-Wimplicit-function-declaration]
  167 |     _tx_timer_interrupt();
      |     ^~~~~~~~~~~~~~~~~~~
      |     _tx_timer_info_get




# 1. Clone the ThreadX submodule (if not already done)                                                                   
  cd /home/kty/work/threadx-esp32c6-project                                                                                
  git submodule update --init --recursive                                                                                  
                                                            
  # 2. Source ESP-IDF environment                                                                                          
  . $IDF_PATH/export.sh                                     

  # 3. Set target to ESP32-C6
  idf.py set-target esp32c6

  # 4. Build
  idf.py build

  # 5. Flash (adjust port if needed)
  idf.py -p /dev/ttyUSB0 flash

  # 6. Monitor serial output
  idf.py -p /dev/ttyUSB0 monitor

  # (Ctrl+] to exit monitor)

idf.py -p /dev/ttyACM0 monitor