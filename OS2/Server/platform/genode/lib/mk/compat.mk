include $(REP_DIR)/mk/osfree.mk

CC_CXX_WARN_STRICT =
SRC_CC = log.cc fileprov.cc err.cc dataspace.cc rm.cc
SRC_C  = cfgparser.c env.c path.c token.c string.c
LIBS = base libc stdcxx

SHARED_LIB = yes

vpath %.c  $(OS3_DIR)/shared/lib/compat
vpath %.cc $(REP_DIR)/src/lib/compat
