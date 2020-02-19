/* ip_said tests, for libreswan
 *
 * Copyright (C) 2000  Henry Spencer.
 * Copyright (C) 2019 Andrew Cagney
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/lgpl-2.1.txt>.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
 * License for more details.
 *
 */

#include <stdio.h>
#include <string.h>

#include "ip_said.h"
#include "ipcheck.h"

static void check_str_said(void)
{
	static const struct test {
		char *in;
		char *out;	/* NULL means error expected */
		bool fudge;
	} tests[] = {
		{ "esp257@1.2.3.0", "esp.101@1.2.3.0" },
		{ "ah0x20@1.2.3.4", "ah.20@1.2.3.4" },
		{ "tun20@1.2.3.4", "tun.14@1.2.3.4" },
		{ "comp20@1.2.3.4", "comp.14@1.2.3.4" },
		{ "esp257@::1", "esp:101@::1" },
		{ "esp257@0bc:12de::1", "esp:101@bc:12de::1" },
		{ "esp78@1049:1::8007:2040", "esp:4e@1049:1::8007:2040" },
		{ "esp0x78@1049:1::8007:2040", "esp:78@1049:1::8007:2040" },
		{ "ah78@1049:1::8007:2040", "ah:4e@1049:1::8007:2040" },
		{ "ah0x78@1049:1::8007:2040", "ah:78@1049:1::8007:2040" },
		{ "tun78@1049:1::8007:2040", "tun:4e@1049:1::8007:2040" },
		{ "tun0x78@1049:1::8007:2040", "tun:78@1049:1::8007:2040" },
		{ "duk99@3ffe:370:400:ff::9001:3001", NULL },
		{ "esp78x@1049:1::8007:2040", NULL },
		{ "esp0x78@1049:1:0xfff::8007:2040", NULL },
		{ "es78@1049:1::8007:2040", NULL },
		{ "", NULL },
		{ "_", NULL },
		{ "ah2.2", NULL },
		{ "goo2@1.2.3.4", NULL },
		{ "esp9@1.2.3.4", "esp.9@1.2.3.4" },
		{ "espp9@1.2.3.4", NULL },
		{ "es9@1.2.3.4", NULL },
		{ "ah@1.2.3.4", NULL },
		{ "esp7x7@1.2.3.4", NULL },
		{ "esp77@1.0x2.3.4", NULL },
		{ PASSTHROUGHNAME, PASSTHROUGH4NAME },
		{ PASSTHROUGH6NAME, PASSTHROUGH6NAME },
		{ "%pass", "%pass" },
		{ "int256@0.0.0.0", "%pass" },
		{ "%drop", "%drop" },
		{ "int257@0.0.0.0", "%drop" },
		{ "%reject", "%reject" },
		{ "int258@0.0.0.0", "%reject" },
		{ "%hold", "%hold" },
		{ "int259@0.0.0.0", "%hold" },
		{ "%trap", "%trap" },
		{ "int260@0.0.0.0", "%trap" },
		{ "%trapsubnet", "%trapsubnet" },
		{ "int261@0.0.0.0", "%trapsubnet" },
		/* was "int.106@0.0.0.0" */
		{ "int262@0.0.0.0", "%unk-262" },
		{ "esp9@1.2.3.4", "unk.9@1.2.3.4", .fudge = true, },
		{ "unk77.9@1.2.3.4", NULL, },

		/* buffer size? */
		{ "esp.3a7292a2@192.1.2.24", "esp.3a7292a2@192.1.2.24" },
		{ "esp:3a7292a2@1000:2000:3000:4000:5000:6000:7000:8000", "esp:3a7292a2@1000:2000:3000:4000:5000:6000:7000:8000", },
	};

#define PRINT_SA(FILE, FMT, ...)					\
	PRINT(FILE, " '%s' fudge: %s"FMT,				\
	      t->in, bool_str(t->fudge),##__VA_ARGS__);

#define FAIL_SA(FMT, ...)						\
	{								\
		fails++;						\
		PRINT_SA(stderr, " "FMT" ("PRI_WHERE")",##__VA_ARGS__,	\
			 pri_where(HERE));				\
		continue;						\
	}

	for (size_t ti = 0; ti < elemsof(tests); ti++) {
		const struct test *t = &tests[ti];
		PRINT_SA(stdout, "");

		/* convert it *to* internal format */
		ip_said sa;
		err_t err = ttosa(t->in, strlen(t->in), &sa);
		if (err != NULL) {
			if (t->out != NULL) {
				FAIL_SA("ttosa() unexpectedly failed: %s", err);
			} else {
				/* all is good */
				continue;
			}
		} else if (t->out == NULL) {
			FAIL_SA("ttosa() unexpectedly succeeded");
		}

		if (t->fudge) {
			sa.proto = NULL;
		}

		/* now convert it back */
		said_buf buf;
		const char *out = str_said(&sa, &buf);
		if (out == NULL) {
			FAIL_SA("str_said() failed");
		} else if (!strcaseeq(t->out, out)) {
			FAIL_SA("str_said() returned '%s', expected '%s'",
				out, t->out);
		}
	}
}

void ip_said_check(void)
{
	check_str_said();
}
