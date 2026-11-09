/*
 * gpsfeed: hand the modem's GPS fixes to locationd (GPS step 2).
 *
 *   gpsfeed [-f FILE] [-q]      runs until killed (boot job: hotdogb-launchd-job.py add gpsfeed --keep -- ...)
 *
 * wland's QMI LOC client (phone/wifi/gnss.c) writes the latest fix to /var/local/gps.fix once a second
 * (one key=value line: utc_ms lat lon hacc alt vacc speed course sats_used sats_view status wland_ms).
 * Every new good fix (status 0, or an accuracy between 0 and 5 km) goes to locationd through CoreLocation's
 * CLSimulationManager - the path Xcode's "simulate location" uses; locationd only takes it from a client with
 * the com.apple.locationd.simulation entitlement (gpsfeed-ents.plist). wland rewrites the file every second
 * with its last fix, so a fix counts as new only when its GPS time (utc_ms) advances; none for 15 s stops the
 * simulation, so apps see "no location" instead of a frozen one. Timestamps are the phone's clock (now), not
 * the GPS time: locationd compares them with its own clock. Coordinates are never logged.
 */
#import <Foundation/Foundation.h>
#import <CoreLocation/CoreLocation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

@interface CLSimulationManager : NSObject
- (void)clearSimulatedLocations;
- (void)appendSimulatedLocation:(CLLocation *)location;
- (void)startLocationSimulation;
- (void)stopLocationSimulation;
- (void)flush;
@end

#define STALE_SECS	15

struct fix {
	double utc_ms, lat, lon, hacc, alt, vacc, speed, course;
	int have_alt, have_vacc, have_speed, have_course, sats_used, sats_view;
	unsigned status;
};

static int get(const char *line, const char *key, double *v)
{
	char pat[32];
	snprintf(pat, sizeof(pat), " %s=", key);
	const char *p = strstr(line, pat);	/* line starts with a space (read_fix), so the first key matches too */
	if (!p)
		return 0;
	*v = strtod(p + strlen(pat), NULL);
	return 1;
}

static int read_fix(const char *path, struct fix *f)
{
	char line[512];
	FILE *fp = fopen(path, "r");
	if (!fp)
		return -1;
	line[0] = ' ';
	int ok = fgets(line + 1, sizeof(line) - 1, fp) != NULL;
	fclose(fp);
	if (!ok)
		return -1;
	double v;
	memset(f, 0, sizeof(*f));
	get(line, "utc_ms", &f->utc_ms);
	if (!get(line, "lat", &f->lat) || !get(line, "lon", &f->lon) || !get(line, "hacc", &f->hacc))
		return -1;
	f->have_alt = get(line, "alt", &f->alt);
	f->have_vacc = get(line, "vacc", &f->vacc);
	f->have_speed = get(line, "speed", &f->speed);
	f->have_course = get(line, "course", &f->course);
	if (get(line, "sats_used", &v))
		f->sats_used = (int)v;
	if (get(line, "sats_view", &v))
		f->sats_view = (int)v;
	if (get(line, "status", &v))
		f->status = (unsigned)v;
	return 0;
}

static int good(const struct fix *f)
{
	if (f->lat == 0 && f->lon == 0)
		return 0;
	if (f->lat < -90 || f->lat > 90 || f->lon < -180 || f->lon > 180)
		return 0;
	return f->status == 0 ? f->hacc > 0 : (f->hacc > 0 && f->hacc <= 5000);
}

int main(int argc, char **argv)
{
	const char *path = "/var/local/gps.fix";
	int quiet = 0, c;
	while ((c = getopt(argc, argv, "f:q")) != -1) {
		if (c == 'f')
			path = optarg;
		else if (c == 'q')
			quiet = 1;
		else {
			fprintf(stderr, "usage: gpsfeed [-f FILE] [-q]\n");
			return 2;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	@autoreleasepool {
		Class cls = NSClassFromString(@"CLSimulationManager");
		if (!cls) {
			printf("gpsfeed: no CLSimulationManager in CoreLocation\n");
			return 1;
		}
		CLSimulationManager *sim = [cls new];
		[sim stopLocationSimulation];
		[sim clearSimulatedLocations];
		[sim flush];
		printf("#### GPS FEED STARTED (%s)\n", path);
		struct timespec last_mtime = { 0, 0 };
		double last_utc = 0;
		time_t last_sent = 0, last_log = 0;
		unsigned sent = 0, skipped = 0;
		int running = 0;
		for (;;) {
			@autoreleasepool {
				struct stat st;
				time_t now = time(NULL);
				if (stat(path, &st) == 0 && (st.st_mtimespec.tv_sec != last_mtime.tv_sec ||
							      st.st_mtimespec.tv_nsec != last_mtime.tv_nsec)) {
					last_mtime = st.st_mtimespec;
					struct fix f;
					int rd = read_fix(path, &f);
					if (rd == 0 && f.utc_ms == last_utc) {
						/* the same fix rewritten: not new */
					} else if (rd == 0 && good(&f)) {
						last_utc = f.utc_ms;
						CLLocationCoordinate2D co = CLLocationCoordinate2DMake(f.lat, f.lon);
						double vacc = f.have_alt && f.have_vacc && f.vacc > 0 ? f.vacc : -1;
						CLLocation *loc = [[CLLocation alloc]
							initWithCoordinate:co
								  altitude:f.have_alt ? f.alt : 0
							horizontalAccuracy:f.hacc
							  verticalAccuracy:vacc
								    course:f.have_course ? f.course : -1
								     speed:f.have_speed ? f.speed : -1
								 timestamp:[NSDate date]];
						[sim clearSimulatedLocations];
						[sim appendSimulatedLocation:loc];
						[sim flush];
						if (!running) {
							[sim startLocationSimulation];
							running = 1;
							printf("#### %s LOCATION SENT TO LOCATIOND: +-%.0f m, %d of %d satellites\n",
							       sent ? "GPS BACK:" : "FIRST", f.hacc, f.sats_used, f.sats_view);
						}
						sent++;
						last_sent = now;
					} else {
						skipped++;
					}
				}
				if (running && last_sent && now - last_sent > STALE_SECS) {
					[sim stopLocationSimulation];
					[sim clearSimulatedLocations];
					[sim flush];
					running = 0;
					printf("#### GPS FEED STALE: no good fix for %d s, simulation stopped\n", STALE_SECS);
				}
				if (!quiet && now - last_log >= 60) {
					last_log = now;
					printf("gpsfeed: %u fixes sent, %u skipped (no fix yet / poor), simulation %s\n", sent,
					       skipped, running ? "on" : "off");
				}
			}
			sleep(1);
		}
	}
}
