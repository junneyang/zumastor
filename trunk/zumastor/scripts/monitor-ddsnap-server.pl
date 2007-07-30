#!/usr/bin/perl
#
# gnome-spyware.pl : Monitor system usage
#
# Copyright (C) 2006 by Callum McKenzie
#
# Time-stamp: <2006-12-19 20:30:51 callum>
#

use POSIX qw(strftime);
use IO::Handle;
use Getopt::Std;

$logfile = "$ENV{HOME}/.gnome-spyware-log";
%options = ();

getopt ("f", \%options);
$logfile = $options{f} if defined $options{f};

#open (LOGFILE, ">>" . $logfile) or die "Could not open the log file: $!";
open (LOGFILE, ">&STDOUT") or die "Couldn't dup stdout: $!";
select LOGFILE; $| = 1;

$starttime = time;

sub scan_process {
	my $pid = $_[0];
	my $elapsed = $_[1];
	my $vol = $_[2];

	my $basename = "/proc/" . $pid . "/";
	my $rss = 0;
	my $private_dirty = 0;
	my $private_clean = 0;
	my $shared_dirty = 0;
	my $shared_clean = 0;
	open SMAPS, "<" . $basename . "smaps" or return;
	while (<SMAPS>) {
		if (/^Rss:\D*(\d+)\D*$/) {
			$rss += $1;
		}
		if (/^Private_Clean:\D*(\d+)\D*$/) {
			$private_clean += $1;
		}
		if (/^Private_Dirty:\D*(\d+)\D*$/) {
			$private_dirty += $1;
		}
		if (/^Shared_Clean:\D*(\d+)\D*$/) {
			$shared_clean += $1;
		}
		if (/^Shared_Dirty:\D*(\d+)\D*$/) {
			$shared_dirty += $1;
		}
	}
	print LOGFILE "$vol $elapsed $shared_clean $shared_dirty $private_clean $private_dirty $rss\n";
	LOGFILE->flush();
}

while (1) {
	unless (opendir(SDIR, "/var/run/zumastor/servers")) {
		die("Couldn't open server piddir: $!");
	}
	@servers = grep /^\w+$/, readdir(SDIR);
	closedir SDIR;
	foreach $server (@servers) {
		next unless open(PID, "/var/run/zumastor/$server-server.pid");
		$ddsnappid = <PID>;
		close(PID);
		chomp $ddsnappid;
		scan_process($ddsnappid, (time - $starttime), $server);
	}
	sleep (30);
}
