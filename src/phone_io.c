#include "phone_io.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846
/* Flash ROM has a device callback even in array mode. bus_peek deliberately
   rejects devices; the verified constant table may also be read from a fast
   array-mode ROM without issuing extra guest/device accesses. */
static int table_word(Bus *bus,uint32_t address,uint32_t *value)
{
    unsigned i;if(bus_peek(bus,address,2,value))return 1;
    for(i=0;i<bus->count;++i){Region *r=&bus->regions[i];uint64_t end=(uint64_t)address+2;
        if(r->readonly&&r->data&&r->fast&&address>=r->base&&end<=(uint64_t)r->base+r->size){
            uint16_t word;memcpy(&word,r->data+address-r->base,2);*value=word;return 1;}}
    return 0;
}
void phone_io_observe(PhoneIo *io,Arm7 *c){unsigned i;uint32_t note[2],v,a;
    if(!io->configured||!(c->cpsr&ARM_T))return;
    if(!io->hardware_led&&c->r[15]==io->gpio_entry&&c->r[1]<=1){
        for(i=0;i<3;++i)if(c->r[0]==io->led_pins[i]){unsigned old=io->led;
            io->led_tick=c->bus->ticks;
            io->led=(io->led&~(1u<<i))|(c->r[1]<<i);if(old!=io->led)++io->led_changes;}return;
    }
    if(c->r[15]==io->tone_stop){io->amplitude[0]=io->amplitude[1]=0;++io->tone_changes;return;}
    if(c->r[15]!=io->tone_entry)return;
    if(!bus_peek(c->bus,c->r[0],1,&note[0])||!bus_peek(c->bus,c->r[1],1,&note[1])||note[0]>=51||note[1]>=51)return;
    for(i=0;i<2;++i){
        if(!note[i]){io->amplitude[i]=0;continue;}
        if(!table_word(c->bus,io->tone_table+note[i]*4,&v)||
           !table_word(c->bus,io->tone_table+note[i]*4+2,&a))continue;
        /* DSP second-order oscillator coefficient: Q15 cos(2*pi*f/8000).
           The companion word is the oscillator excitation/amplitude. */
        io->step[i]=acos((double)(int16_t)v/32768.0)*8000.0/AUDIO_RATE;
        io->amplitude[i]=(double)(int16_t)a*4.0;
    }
    ++io->tone_changes;
}

void phone_io_gpio_write(void *context,unsigned mask,unsigned value,uint64_t ticks)
{
    PhoneIo *io=context;unsigned i,relevant=0,state=0;
    for(i=0;i<3;++i){unsigned pin=1u<<io->led_pins[i];
        relevant|=pin;if(value&pin)state|=1u<<i;}
    if(!(mask&relevant))return;
    io->led_tick=ticks;if(io->led!=state)++io->led_changes;io->led=state;
}

int phone_io_open(PhoneIo *io, int speakers, const char *wav)
{
    if (!io->configured) return 1;
    phone_io_close(io);
    io->output = audio_output_open(1, speakers, wav);
    io->frames = io->nonzero = io->fraction = io->last_ns = 0;
    return io->output != NULL;
}

int phone_io_live(const PhoneIo *io)
{
    return audio_output_live(io->output);
}

void phone_io_pause(PhoneIo *io, int pause)
{
    audio_output_pause(io->output, pause);
}

static int16_t next_sample(PhoneIo *io)
{
    double value=0;int sample;unsigned i;
    /* Most song playback has no CPU tone. Avoid two transcendental calls per
       sample for an entirely silent channel. Snap the inaudible release tail. */
    for(i=0;i<2;++i){
        if(!io->amplitude[i]&&fabs(io->render_amplitude[i])<0.01){io->render_amplitude[i]=0;continue;}
        io->render_amplitude[i]+=(io->amplitude[i]-io->render_amplitude[i])*0.02;
        value+=sin(io->phase[i])*io->render_amplitude[i];io->phase[i]+=io->step[i];
        if(io->phase[i]>=2*PI)io->phase[i]-=2*PI;
    }
    sample=(int)value;if(sample>32767)sample=32767;if(sample< -32768)sample=-32768;
    ++io->frames;if(sample)++io->nonzero;return (int16_t)sample;
}
/* The MA-3 renderer mixes CPU/DSP tones in the same emulated-time pass. */
void phone_io_mix(void *context,int16_t *left,int16_t *right,unsigned frames)
{
    PhoneIo *io=context;unsigned i;if(!io->configured)return;
    for(i=0;i<frames;++i){int16_t sample=next_sample(io);int l=left[i]+sample,r=right[i]+sample;
        left[i]=(int16_t)(l>32767?32767:l< -32768?-32768:l);
        right[i]=(int16_t)(r>32767?32767:r< -32768?-32768:r);
        if(io->output)audio_output_write(io->output,&sample,1);}
}
void phone_io_advance(PhoneIo *io,uint64_t ns)
{
    uint64_t count;
    if(!io->output||io->mixed||ns<=io->last_ns)return;
    count=io->fraction+(ns-io->last_ns)*AUDIO_RATE;io->last_ns=ns;
    io->fraction=count%1000000000;count/=1000000000;
    while(count) {
        int16_t samples[AUDIO_BLOCK];
        unsigned i,frames=count>AUDIO_BLOCK?AUDIO_BLOCK:(unsigned)count;
        for(i=0;i<frames;++i)samples[i]=next_sample(io);
        if(!audio_output_write(io->output,samples,frames))break;
        count-=frames;
    }
}

void phone_io_close(PhoneIo *io)
{
    audio_output_close(io->output);
    io->output = NULL;
}
