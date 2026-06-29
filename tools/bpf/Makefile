# SPDX-License-Identifier: GPL-2.0
include ../scripts/Makefile.include

prefix ?= /usr/local

LEX = flex
YACC = bison
MAKE = make
INSTALL ?= install

CFLAGS += -Wall -O2
CFLAGS += -Wall -O2
CFLAGS += -D__EXPORTED_HEADERS__ -I$(srctree)/tools/include/uapi \
          -I$(srctree)/tools/include \
          -I$(srctree)/tools/bpf/include

# This will work when bpf is built in tools env. where srctree
# isn't set and when invoked from selftests build, where srctree
# is set to ".". building_out_of_srctree is undefined for in srctree
# builds
ifeq ($(srctree),)
update_srctree := 1
endif
ifndef building_out_of_srctree
update_srctree := 1
endif
ifeq ($(update_srctree),1)
srctree := $(patsubst %/,%,$(dir $(CURDIR)))
srctree := $(patsubst %/,%,$(dir $(srctree)))
endif

FEATURE_USER = .bpf
FEATURE_TESTS = libbfd disassembler-four-args disassembler-init-styled
FEATURE_DISPLAY = libbfd

check_feat := 1
NON_CHECK_FEAT_TARGETS := clean bpftool_clean runqslower_clean resolve_btfids_clean
ifdef MAKECMDGOALS
ifeq ($(filter-out $(NON_CHECK_FEAT_TARGETS),$(MAKECMDGOALS)),)
  check_feat := 0
endif
endif

ifeq ($(check_feat),1)
ifeq ($(FEATURES_DUMP),)
include $(srctree)/tools/build/Makefile.feature
else
include $(FEATURES_DUMP)
endif
endif

ifeq ($(feature-disassembler-four-args), 1)
CFLAGS += -DDISASM_FOUR_ARGS_SIGNATURE
endif
ifeq ($(feature-disassembler-init-styled), 1)
CFLAGS += -DDISASM_INIT_STYLED
endif

$(OUTPUT)%.yacc.c: $(srctree)/tools/bpf/%.y
	$(QUIET_BISON)$(YACC) -o $@ -d $<

$(OUTPUT)%.lex.c: $(srctree)/tools/bpf/%.l
	$(QUIET_FLEX)$(LEX) -o $@ $<

$(OUTPUT)%.o: $(srctree)/tools/bpf/%.c
	$(QUIET_CC)$(CC) $(CFLAGS) -c -o $@ $<

$(OUTPUT)%.yacc.o: $(OUTPUT)%.yacc.c
	$(QUIET_CC)$(CC) $(CFLAGS) -c -o $@ $<
$(OUTPUT)%.lex.o: $(OUTPUT)%.lex.c
	$(QUIET_CC)$(CC) $(CFLAGS) -c -o $@ $<

PROGS = $(OUTPUT)bpf_jit_disasm $(OUTPUT)bpf_dbg $(OUTPUT)bpf_asm

all: $(PROGS) bpftool runqslower

$(OUTPUT)bpf_jit_disasm: CFLAGS += -DPACKAGE='bpf_jit_disasm'
$(OUTPUT)bpf_jit_disasm: $(OUTPUT)bpf_jit_disasm.o
	$(QUIET_LINK)$(CC) $(CFLAGS) -o $@ $^ -lopcodes -lbfd -ldl

$(OUTPUT)bpf_dbg: $(OUTPUT)bpf_dbg.o
	$(QUIET_LINK)$(CC) $(CFLAGS) -o $@ $^ -lreadline

$(OUTPUT)bpf_asm: $(OUTPUT)bpf_asm.o $(OUTPUT)bpf_exp.yacc.o $(OUTPUT)bpf_exp.lex.o
	$(QUIET_LINK)$(CC) $(CFLAGS) -o $@ $^

$(OUTPUT)bpf_exp.lex.c: $(OUTPUT)bpf_exp.yacc.c
$(OUTPUT)bpf_exp.yacc.o: $(OUTPUT)bpf_exp.yacc.c
$(OUTPUT)bpf_exp.lex.o: $(OUTPUT)bpf_exp.lex.c

clean: bpftool_clean runqslower_clean resolve_btfids_clean
	$(call QUIET_CLEAN, bpf-progs)
	$(Q)$(RM) -r -- $(OUTPUT)*.o $(OUTPUT)bpf_jit_disasm $(OUTPUT)bpf_dbg \
	       $(OUTPUT)bpf_asm $(OUTPUT)bpf_exp.yacc.* $(OUTPUT)bpf_exp.lex.*
	$(call QUIET_CLEAN, core-gen)
	$(Q)$(RM) -- $(OUTPUT)FEATURE-DUMP.bpf
	$(Q)$(RM) -r -- $(OUTPUT)feature

install: $(PROGS) bpftool_install
	$(call QUIET_INSTALL, bpf_jit_disasm)
	$(Q)$(INSTALL) -m 0755 -d $(DESTDIR)$(prefix)/bin
	$(Q)$(INSTALL) $(OUTPUT)bpf_jit_disasm $(DESTDIR)$(prefix)/bin/bpf_jit_disasm
	$(call QUIET_INSTALL, bpf_dbg)
	$(Q)$(INSTALL) $(OUTPUT)bpf_dbg $(DESTDIR)$(prefix)/bin/bpf_dbg
	$(call QUIET_INSTALL, bpf_asm)
	$(Q)$(INSTALL) $(OUTPUT)bpf_asm $(DESTDIR)$(prefix)/bin/bpf_asm

bpftool:
	$(call descend,bpftool)

bpftool_install:
	$(call descend,bpftool,install)

bpftool_clean:
	$(call descend,bpftool,clean)

runqslower:
	$(call descend,runqslower)

runqslower_clean:
	$(call descend,runqslower,clean)

resolve_btfids:
	$(call descend,resolve_btfids)

resolve_btfids_clean:
	$(call descend,resolve_btfids,clean)

.PHONY: all install clean bpftool bpftool_install bpftool_clean \
	runqslower runqslower_clean \
	resolve_btfids resolve_btfids_clean \
	ivh_atc lhp_movable lhp_cstime lhp_waittime MY_ivh_atc

# --- ivh_atc self-contained build ---
IVH_BPFTOOL  := bpftool
IVH_ARCH     := x86
IVH_LIBBPF_INC := /usr/local/include
IVH_LIBBPF_LIB := /usr/local/lib64

ivh_atc: ivh_atc.c ivh_atc.skel.h
	cc -O2 \
		-I$(srctree)/tools/include/uapi \
		-I$(IVH_LIBBPF_INC) \
		-o $@ $< \
		$(IVH_LIBBPF_LIB)/libbpf.a -lelf -lz

ivh_atc.skel.h: ivh_atc.bpf.o
	$(IVH_BPFTOOL) gen skeleton $< name ivh_atc > $@

ivh_atc.bpf.o: ivh_atc.bpf.c
	clang -O2 -g -target bpf -D__TARGET_ARCH_$(IVH_ARCH) \
		-I$(srctree) -I./include \
		-c $< -o $@

# --- MY_ivh_atc self-contained build ---
MY_ivh_atc: MY_ivh_atc.c MY_ivh_atc.skel.h
	cc -O2 \
		-I$(srctree)/tools/include/uapi \
		-I$(IVH_LIBBPF_INC) \
		-o $@ $< \
		$(IVH_LIBBPF_LIB)/libbpf.a -lelf -lz

MY_ivh_atc.skel.h: MY_ivh_atc.bpf.o
	$(IVH_BPFTOOL) gen skeleton $< name MY_ivh_atc > $@

MY_ivh_atc.bpf.o: MY_ivh_atc.bpf.c
	clang -O2 -g -target bpf -D__TARGET_ARCH_$(IVH_ARCH) \
		-I$(srctree) -I./include \
		-c $< -o $@

# --- lhp_movable self-contained build ---
lhp_movable: lhp_movable.c lhp_movable.skel.h
	cc -O2 \
		-I$(srctree)/tools/include/uapi \
		-I$(IVH_LIBBPF_INC) \
		-o $@ $< \
		$(IVH_LIBBPF_LIB)/libbpf.a -lelf -lz

lhp_movable.skel.h: lhp_movable.bpf.o
	$(IVH_BPFTOOL) gen skeleton $< name lhp_movable > $@

lhp_movable.bpf.o: lhp_movable.bpf.c
	clang -O2 -g -target bpf -D__TARGET_ARCH_$(IVH_ARCH) \
		-I$(srctree) -I./include \
		-c $< -o $@

# --- lhp_cstime self-contained build ---
lhp_cstime: lhp_cstime.c lhp_cstime.skel.h
	cc -O2 \
		-I$(srctree)/tools/include/uapi \
		-I$(IVH_LIBBPF_INC) \
		-o $@ $< \
		$(IVH_LIBBPF_LIB)/libbpf.a -lelf -lz

lhp_cstime.skel.h: lhp_cstime.bpf.o
	$(IVH_BPFTOOL) gen skeleton $< name lhp_cstime > $@

lhp_cstime.bpf.o: lhp_cstime.bpf.c
	clang -O2 -g -target bpf -D__TARGET_ARCH_$(IVH_ARCH) \
		-I$(srctree) -I./include \
		-c $< -o $@

# --- lhp_waittime self-contained build ---
lhp_waittime: lhp_waittime.c lhp_waittime.skel.h
	cc -O2 \
		-I$(srctree)/tools/include/uapi \
		-I$(IVH_LIBBPF_INC) \
		-o $@ $< \
		$(IVH_LIBBPF_LIB)/libbpf.a -lelf -lz

lhp_waittime.skel.h: lhp_waittime.bpf.o
	$(IVH_BPFTOOL) gen skeleton $< name lhp_waittime > $@

lhp_waittime.bpf.o: lhp_waittime.bpf.c
	clang -O2 -g -target bpf -D__TARGET_ARCH_$(IVH_ARCH) \
		-I$(srctree) -I./include \
		-c $< -o $@
