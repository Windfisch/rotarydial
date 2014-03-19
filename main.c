#include <avr/io.h>
#include <avr/delay.h>


int main (void)
{
	DDRC=0x00;
	PORTC=0x00;

	DDRB=0xFF;



	while(1)
	{
               PORTB|=1;
		_delay_ms(500);
		PORTB&= ~1;
		_delay_ms(500);

	}


	return 0; // never reached
}
