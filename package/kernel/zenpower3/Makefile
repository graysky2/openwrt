include $(TOPDIR)/rules.mk

PKG_NAME:=zenpower3
PKG_RELEASE:=1

PKG_LICENSE:=GPLv2
PKG_LICENSE_FILES:=

PKG_SOURCE_URL:=https://git.exozy.me/a/zenpower3
PKG_SOURCE_PROTO:=git
PKG_SOURCE_DATE:=2022-09-17
PKG_SOURCE_VERSION:=c176fdb0d5bcba6ba2aba99ea36812e40f47751f

PKG_MAINTAINER:=John Audia <therealgraysky@proton.me>
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/zenpower3
  SUBMENU:=Hardware Monitoring Support
  TITLE:=Zen3 driver to read temps, voltage(SVI2), current(SVI2) and power(SVI2)
  DEPENDS:=@PCI_SUPPORT
  FILES:=$(PKG_BUILD_DIR)/zenpower.ko
KCONFIG:=
endef

define KernelPackage/zenpower3/description
Zenpower3 is a Linux kernel driver for reading temperature, voltage(SVI2), current(SVI2) and power(SVI2)
for AMD Zen family CPUs, now with Zen 3 support!

Make sure that your Linux kernel have support for your CPUs as Zenpower is using kernel function
amd_smn_read to read values from SMN. A fallback method (which may or may not work!) will be used
when it is detected that kernel function amd_smn_read lacks support for your CPU.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef
$(eval $(call KernelPackage,zenpower3))
