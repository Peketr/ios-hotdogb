/*
 * bluetool-shim.dylib: loaded into BlueTool only (DYLD_INSERT_LIBRARIES in the com.apple.BlueTool launchd job).
 *
 * BlueTool runs the override scripts /etc/bluetool/<HWModel>.<boot|init>.script only when MobileGestalt's
 * ReleaseType is "Vendor" or "Internal" (iOS 16.7 usr/sbin/BlueTool, 0x100016054); otherwise it uses its built-in
 * Broadcom setup chosen by ProductHash and chip, which cannot work with the WCN3998 behind btd. This answers
 * ReleaseType = "Vendor" and leaves every other key alone. The Galaxy S9 port (Scion-Project PR 171) does the same.
 * HWModel here is D22AP (ADT target-sub-type), so the scripts are D22AP.boot.script / D22AP.init.script.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);
extern CFTypeRef MGCopyAnswerWithError(CFStringRef key, void *unused, int *error);

#define DYLD_INTERPOSE(_new, _old) \
	__attribute__((used)) static struct { const void *n, *o; } _interpose_##_old \
	__attribute__((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&_new, (const void *)(unsigned long)&_old };

static int is_release_type(CFStringRef key)
{
	return key && CFGetTypeID(key) == CFStringGetTypeID() && CFEqual(key, CFSTR("ReleaseType"));
}

static CFTypeRef shim_MGCopyAnswer(CFStringRef key)
{
	if (is_release_type(key)) {
		static int logged;
		if (!logged++)
			fprintf(stderr, "bluetool-shim: ReleaseType = Vendor\n");
		return CFRetain(CFSTR("Vendor"));
	}
	return MGCopyAnswer(key);
}

static CFTypeRef shim_MGCopyAnswerWithError(CFStringRef key, void *unused, int *error)
{
	if (is_release_type(key)) {
		if (error)
			*error = 0;
		return CFRetain(CFSTR("Vendor"));
	}
	return MGCopyAnswerWithError(key, unused, error);
}

DYLD_INTERPOSE(shim_MGCopyAnswer, MGCopyAnswer)
DYLD_INTERPOSE(shim_MGCopyAnswerWithError, MGCopyAnswerWithError)
