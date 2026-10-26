#pragma once
/* Physical pixels, independent of the legacy logical UI scale. */
#define TOUCH_TARGET_MIN 48
#define TOUCH_GAP_MIN 12
static int touch_at_least(int value,int minimum){return value<minimum ? minimum:value;}
static int touch_keyboard_height(int height,int scaled)
{
  int desired=touch_at_least(scaled,224),limit=height/2;
  return desired>limit ? limit:desired;
}
static int touch_settings_step(int original,bool stacked)
{ return touch_at_least(original,TOUCH_TARGET_MIN+TOUCH_GAP_MIN)+(stacked ? 36:0); }
