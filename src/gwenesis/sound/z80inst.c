#include "z80inst.h"

int bus_ack = 0;
int reset = 0;

inline __attribute__((always_inline))
void z80_write_ctrl(unsigned int address, unsigned int value)
{
    if (address == 0x1100) // BUSREQ
    {
        if (value)
        {
            bus_ack = 1;
        }
        else
        {
            bus_ack = 0;
        }
    }
    else if (address == 0x1200) // RESET
    {
        if (value)
        {
            reset = 1;
        }
        else
        {
            reset = 0;
        }
    }
}

inline __attribute__((always_inline))
unsigned int z80_read_ctrl(unsigned int address)
{
    if (address == 0x1100)
    {
        return !(reset && bus_ack);
    }
    return 0;
}
