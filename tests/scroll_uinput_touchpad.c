#include <linux/uinput.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
static int fd;
static void ev(int type,int code,int value) { struct input_event e={.type=type,.code=code,.value=value}; if(write(fd,&e,sizeof(e))!=sizeof(e)) {perror("event");exit(1);} }
static void axis(int code,int min,int max,int res) {ioctl(fd,UI_SET_ABSBIT,code);struct uinput_abs_setup a={.code=code,.absinfo={.minimum=min,.maximum=max,.resolution=res}};if(ioctl(fd,UI_ABS_SETUP,&a)) {perror("axis");exit(1);} }
static void contact(int slot,int id,int x,int y) {ev(EV_ABS,ABS_MT_SLOT,slot);ev(EV_ABS,ABS_MT_TRACKING_ID,id);if(id>=0) {ev(EV_ABS,ABS_MT_POSITION_X,x);ev(EV_ABS,ABS_MT_POSITION_Y,y);} }
int main(int argc,char**argv) {
 fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK);if(fd<0){perror("uinput");return 1;}
 ioctl(fd,UI_SET_EVBIT,EV_KEY);int keys[]={BTN_LEFT,BTN_TOUCH,BTN_TOOL_FINGER,BTN_TOOL_DOUBLETAP,BTN_TOOL_TRIPLETAP};for(int i=0;i<5;i++)ioctl(fd,UI_SET_KEYBIT,keys[i]);
 ioctl(fd,UI_SET_EVBIT,EV_ABS);ioctl(fd,UI_SET_PROPBIT,INPUT_PROP_POINTER);ioctl(fd,UI_SET_PROPBIT,INPUT_PROP_BUTTONPAD);
 axis(ABS_X,0,12000,100);axis(ABS_Y,0,7000,100);axis(ABS_MT_SLOT,0,1,0);axis(ABS_MT_TRACKING_ID,0,65535,0);axis(ABS_MT_POSITION_X,0,12000,100);axis(ABS_MT_POSITION_Y,0,7000,100);axis(ABS_PRESSURE,0,255,0);
 struct uinput_setup setup={.id={.bustype=BUS_USB,.vendor=0x1209,.product=0xcafe,.version=1}};strcpy(setup.name,"HyprCapture Audit Touchpad");
 if(ioctl(fd,UI_DEV_SETUP,&setup)||ioctl(fd,UI_DEV_CREATE)){perror("create");return 1;}
 puts("created");fflush(stdout);sleep(3);
 char cmd[128];while(fgets(cmd,sizeof(cmd),stdin)) {
 int distance=atoi(cmd);if(!distance)break;
 ev(EV_KEY,BTN_TOUCH,1);ev(EV_KEY,BTN_TOOL_DOUBLETAP,1);ev(EV_ABS,ABS_PRESSURE,40);ev(EV_ABS,ABS_X,5000);ev(EV_ABS,ABS_Y,3500);contact(0,10,5000,3500);contact(1,11,6500,3500);ev(EV_SYN,SYN_REPORT,0);usleep(50000);
 for(int i=1;i<=60;i++) {int y=3500+distance*i/60;ev(EV_ABS,ABS_Y,y);ev(EV_ABS,ABS_MT_SLOT,0);ev(EV_ABS,ABS_MT_POSITION_Y,y);ev(EV_ABS,ABS_MT_SLOT,1);ev(EV_ABS,ABS_MT_POSITION_Y,y);ev(EV_SYN,SYN_REPORT,0);usleep(10000);}
 contact(0,-1,0,0);contact(1,-1,0,0);ev(EV_KEY,BTN_TOUCH,0);ev(EV_KEY,BTN_TOOL_DOUBLETAP,0);ev(EV_ABS,ABS_PRESSURE,0);ev(EV_SYN,SYN_REPORT,0);puts("gesture complete");fflush(stdout);
 }
 ioctl(fd,UI_DEV_DESTROY);close(fd);return 0;
}
