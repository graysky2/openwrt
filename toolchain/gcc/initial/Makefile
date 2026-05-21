GCC_VARIANT:=initial
GCC_PREPARE=$(CONFIG_USE_MUSL)

include ../common.mk

GCC_CONFIGURE += \
	--with-newlib \
	--with-sysroot=$(TOOLCHAIN_DIR) \
	--enable-languages=c \
	--disable-shared \
	--disable-threads \

define Host/Compile
	+$(GCC_MAKE) $(HOST_JOBS) -C $(GCC_BUILD_DIR) \
		all-build-libiberty \
		all-gcc \
		all-target-libgcc
endef

define Host/Install
	+$(GCC_MAKE) $(HOST_JOBS) -C $(GCC_BUILD_DIR) \
		install-gcc \
		install-target-libgcc

	$(call FixupLibdir,$(TOOLCHAIN_DIR))
	if [ $(GCC_MAJOR_VERSION) -ge 16 ]; then \
	    ar rcs \
	        $(TOOLCHAIN_DIR)/lib/gcc/$(REAL_GNU_TARGET_NAME)/$(PKG_VERSION)/libatomic_asneeded.a; \
	fi
endef

$(eval $(call HostBuild))
