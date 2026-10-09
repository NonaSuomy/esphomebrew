#pragma once
#include <stdint.h>
#include <stddef.h>

// Software-double formatter for Lua's %.14g number-to-string conversion.
// Calling the toolchain's prebuilt double printf/libm uses unsupported D
// instructions on P4; replacing numbers with a placeholder breaks scripts.
static inline size_t pax_format_number(char *out, size_t capacity, double value,
	char format, int precision)
{
	size_t used=0;
	auto put = [&](char c) { if (used+1<capacity) out[used]=c; ++used; };
	union { double number; uint64_t bits; } bits{value};
	if (bits.bits >> 63) { put('-'); value=-value; }
	if ((bits.bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000)) {
		const char *text=(bits.bits & UINT64_C(0xfffffffffffff)) ? "nan" : "inf";
		while (*text) put(*text++);
	} else {
		const bool fixed=format=='f' || format=='F';
		const bool exponent=format=='e' || format=='E';
		if (precision<0) precision=6;
		if (!fixed && !exponent && precision==0) precision=1;
		if (precision>16) precision=16;
		int power=0;
		double normal=value;
		if (normal!=0) {
			while (normal>=10) { normal/=10; ++power; }
			while (normal<1) { normal*=10; --power; }
		}
		if (fixed && power < -precision) {
			normal=power == -precision-1 && normal>=5 ? 1 : 0;
			power=normal ? -precision : 0;
		}
		int count=fixed ? power+precision+1 : exponent ? precision+1 : precision;
		if (count<1) count=1;
		if (count>17) count=17;
		uint64_t scale=1;
		for (int i=1; i<count; ++i) scale*=10;
		uint64_t rounded=(uint64_t)(normal*(double)scale+0.5);
		if (rounded>=scale*10) { rounded/=10; ++power; }
		char digits[17];
		for (int i=count-1; i>=0; --i) { digits[i]='0'+rounded%10; rounded/=10; }
		if (value==0) power=0;
		int length=count;
		if (!fixed && !exponent) while (length>1 && digits[length-1]=='0') --length;
		const bool scientific=exponent || (!fixed && (power<-4 || power>=precision));
		if (scientific) {
			put(digits[0]);
			if (length>1) put('.');
			for (int i=1; i<length; ++i) put(digits[i]);
			put(format=='E' || format=='G' ? 'E' : 'e');
			put(power<0 ? '-' : '+');
			unsigned n=power<0 ? -power : power;
			if (n>=100) put('0'+n/100);
			put('0'+(n/10)%10); put('0'+n%10);
		} else {
			const int point=power+1;
			if (point<=0) put('0');
			for (int i=0; i<point; ++i) put(i<length ? digits[i] : '0');
			const int fraction=fixed ? precision : length-point;
			if (fraction>0) put('.');
			for (int i=0; i<fraction; ++i) {
				const int index=point+i;
				put(index<0 || index>=length ? '0' : digits[index]);
			}
		}
	}
	if (capacity) out[used<capacity ? used : capacity-1]=0;
	return used;
}
