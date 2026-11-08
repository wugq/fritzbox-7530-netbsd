#!/usr/bin/env python3
# Build a FIT image (kernel + fdt + ramdisk) for the old IPQ U-Boot, using dtc only.
# usage: mkfit.py kernel.bin board.dtb ramdisk.fs|- out.itb   (ramdisk optional: '-')
# A kernel named *.gz is marked gzip-compressed: U-Boot unpacks it to the
# load address (the flash edition's kernel, with its memory disk inside).
import sys, zlib, subprocess, os
k, f, r, out = [p if p == '-' else os.path.abspath(p) for p in sys.argv[1:5]]
crc = lambda p: zlib.crc32(open(p, 'rb').read())
kcomp = 'gzip' if k.endswith('.gz') else 'none'
rd_node = '' if r == '-' else f'''		ramdisk-1 {{
			description = "NetBSD 11.1 install ramdisk";
			data = /incbin/("{r}");
			type = "ramdisk"; arch = "arm"; os = "linux"; compression = "none";
			load = <0x88000000>; entry = <0x88000000>;
			hash-1 {{ algo = "crc32"; value = <{crc(r):#010x}>; }};
		}};
'''
rd_ref = '' if r == '-' else ' ramdisk = "ramdisk-1";'
its = f'''/dts-v1/;
/ {{
	description = "NetBSD for FRITZ!Box 7530";
	#address-cells = <1>;
	images {{
		kernel-1 {{
			description = "NetBSD FRITZ7530 kernel";
			data = /incbin/("{k}");
			type = "kernel"; arch = "arm"; os = "linux"; compression = "{kcomp}";
			load = <0x80000040>; entry = <0x80000040>;
			hash-1 {{ algo = "crc32"; value = <{crc(k):#010x}>; }};
		}};
		fdt-1 {{
			description = "FRITZ!Box 7530 DTB (patched for NetBSD)";
			data = /incbin/("{f}");
			type = "flat_dt"; arch = "arm"; compression = "none";
			hash-1 {{ algo = "crc32"; value = <{crc(f):#010x}>; }};
		}};
{rd_node}	}};
	configurations {{
		default = "config@1";
		config@1 {{
			description = "NetBSD";
			kernel = "kernel-1"; fdt = "fdt-1";{rd_ref}
		}};
	}};
}};
'''
open(out + '.its', 'w').write(its)
subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-p', '0', '-o', out, out + '.its'], check=True)
