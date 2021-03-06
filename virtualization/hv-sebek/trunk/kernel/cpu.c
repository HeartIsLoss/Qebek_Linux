/********************************************************************************
* This software is licensed under the GNU General Public License:
* http://www.gnu.org/licenses/gpl.html
*
* MAVMM Project Group:
* Anh M. Nguyen, Nabil Schear, Apeksha Godiyal, HeeDong Jung, et al
*
*********************************************************************************/

#include "bitops.h"
#include "string.h"
#include "failure.h"
#include "serial.h"
#include "msr.h"
#include "cpufeature.h"
#include "svm.h"
#include "cpu.h"
//#include "vmx.h"

void display_cacheinfo ( struct cpuinfo_x86 *c )
{
	unsigned int n, dummy, eax, ebx, ecx, edx;

	n = c->cpuid_max_exfunc;

	if (n >= 0x80000005)
	{
		cpuid(0x80000005, &dummy, &ebx, &ecx, &edx);
		outf("CPU: L1 I Cache: %xK (%x bytes/line), D cache %xK (%x bytes/line)\n",
				edx>>24, edx&0xFF, ecx>>24, ecx&0xFF);
		c->x86_cache_size = (ecx >> 24) + (edx >> 24);
		/* On K8 L1 TLB is inclusive, so don't count it */
		c->x86_tlbsize = 0;
	}

	if (n >= 0x80000006)
	{
		cpuid(0x80000006, &dummy, &ebx, &ecx, &edx);
		//ecx = cpuid_ecx(0x80000006);
		c->x86_cache_size = ecx >> 16;
		c->x86_tlbsize += ((ebx >> 16) & 0xfff) + (ebx & 0xfff);

		outf("CPU: L2 Cache: %xK (%x bytes/line)\n", c->x86_cache_size, ecx & 0xFF);
	}

	if (n >= 0x80000007) 
	{
		//cpuid(0x80000007, &dummy, &dummy, &dummy, &c->x86_power);
		edx = cpuid_edx(0x80000007);
		c->x86_power = edx;
	}

	if (n >= 0x80000008)
	{
		//cpuid(0x80000008, &eax, &dummy, &dummy, &dummy);
		eax = cpuid_eax(0x80000008);
		c->x86_virt_bits = (eax >> 8) & 0xff;
		c->x86_phys_bits = eax & 0xff;
	}
}

static int
get_model_name(struct cpuinfo_x86 *c)
{
	unsigned int *v;

	v = (unsigned int *) c->x86_model_id;
	cpuid(0x80000002, &v[0], &v[1], &v[2], &v[3]);
	cpuid(0x80000003, &v[4], &v[5], &v[6], &v[7]);
	cpuid(0x80000004, &v[8], &v[9], &v[10], &v[11]);
	c->x86_model_id[48] = 0;
	return 1;
}

static void
get_cpu_vendor ( struct cpuinfo_x86 *c )
{
	outf ( "Vendor ID: %s\n", c->x86_vendor_id ); /* [DEBUG] */

	if ( ! strncmp ( c->x86_vendor_id, "AuthenticAMD", 12 ) ) {
		c->x86_vendor = X86_VENDOR_AMD;
	} else if ( ! strncmp ( c->x86_vendor_id, "GenuineIntel", 12 ) ) {
		c->x86_vendor = X86_VENDOR_INTEL;
	} else {
		c->x86_vendor = X86_VENDOR_UNKNOWN;
	}
}

void early_identify_cpu ( struct cpuinfo_x86 *c )
{
	c->x86_cache_size = -1;
	c->x86_vendor = X86_VENDOR_UNKNOWN;
	c->x86_model = c->x86_mask = 0;	/* So far unknown... */
	c->x86_vendor_id[0] = '\0'; /* Unset */
	c->x86_model_id[0] = '\0';  /* Unset */
	c->x86_clflush_size = 64;
	c->x86_cache_alignment = c->x86_clflush_size;
	c->x86_max_cores = 1;
	c->cpuid_max_exfunc = 0;
	memset ( &c->x86_capability, 0, sizeof ( c->x86_capability ) );

	/* Get vendor name */
	cpuid ( 0x00000000,
		(unsigned int *)&c->cpuid_max_stdfunc,
		(unsigned int *)&c->x86_vendor_id[0],
		(unsigned int *)&c->x86_vendor_id[8],
		(unsigned int *)&c->x86_vendor_id[4] );

	get_cpu_vendor ( c );

	/* Initialize the standard set of capabilities */
	/* Note that the vendor-specific code below might override */

	/* Intel-defined flags: level 0x00000001 */
	if ( c->cpuid_max_stdfunc >= 0x00000001 ) {
		u32 tfms;
		u32 misc;
		cpuid(0x00000001, &tfms, &misc, &c->x86_capability[4], &c->x86_capability[0]);

		/* CPU signature, see Intel Processor Identification and the CPUID Instruction
		 * Table 2-3 for detail
		 */
		c->x86 = (tfms >> 8) & 0xf;
		c->x86_model = (tfms >> 4) & 0xf;
		c->x86_mask = tfms & 0xf;
		if (c->x86 >= 6) {
			c->x86 += (tfms >> 20) & 0xff;
			c->x86_model += ((tfms >> 16) & 0xF) << 4;
		}
		if (c->x86_capability[0] & (1<<19)) {
			c->x86_clflush_size = ((misc >> 8) & 0xff) * 8;
		}
	} else {
		/* Have CPUID level 0 only - unheard of */
		c->x86 = 4;
	}

	u32 xlvl;
	/* AMD-defined flags: level 0x80000001 */
	xlvl = cpuid_eax ( 0x80000000 );
	c->cpuid_max_exfunc = xlvl;
	if ( ( xlvl & 0xffff0000 ) == 0x80000000 ) {
		if ( xlvl >= 0x80000001 ) {
			c->x86_capability[1] = cpuid_edx ( 0x80000001 );
			c->x86_capability[6] = cpuid_ecx ( 0x80000001 );
		}
		if ( xlvl >= 0x80000004 ) {
			get_model_name ( c ); /* Default name */
		}
	}

	/* Transmeta-defined flags: level 0x80860001 */
	xlvl = cpuid_eax ( 0x80860000 );
	if ( ( xlvl & 0xffff0000 ) == 0x80860000 ) {
		/* Don't set x86_cpuid_level here for now to not confuse. */
		if ( xlvl >= 0x80860001 ) {
			c->x86_capability[2] = cpuid_edx(0x80860001);
		}
	}
}

