#pragma once

typedef int gpio_num_t;

enum {
  GPIO_NUM_1 = 1,
  GPIO_NUM_2 = 2,
  GPIO_NUM_3 = 3,
  GPIO_NUM_4 = 4,
  GPIO_NUM_5 = 5,
  GPIO_NUM_6 = 6,
  GPIO_NUM_7 = 7,
  GPIO_NUM_14 = 14,
  GPIO_NUM_15 = 15,
  GPIO_NUM_16 = 16,
  GPIO_NUM_17 = 17,
  GPIO_NUM_18 = 18,
  GPIO_NUM_21 = 21,
  GPIO_NUM_35 = 35,
  GPIO_NUM_36 = 36,
  GPIO_NUM_37 = 37,
  GPIO_NUM_38 = 38,
  GPIO_NUM_39 = 39,
  GPIO_NUM_40 = 40,
  GPIO_NUM_41 = 41,
  GPIO_NUM_42 = 42,
  GPIO_NUM_45 = 45,
  GPIO_NUM_48 = 48,
};

int gpio_set_level(gpio_num_t pin, int level);
