#!/usr/bin/env perl

use strict;
use warnings;

my ($strip_comm) = @ARGV;
my $count;
my $comm;
my @frames;

sub flush_sample {
    return unless defined $count;
    return unless @frames;

    my @stack = reverse @frames;
    if (defined $comm && $comm ne '' && (!defined $strip_comm || $comm ne $strip_comm)) {
        unshift @stack, $comm;
    }

    for my $frame (@stack) {
        $frame =~ s/^\s+//;
        $frame =~ s/\s+$//;
        $frame =~ s/;/,/g;
    }

    @stack = grep { $_ ne '' } @stack;
    print join(';', @stack), " $count\n" if @stack;

    undef $count;
    undef $comm;
    @frames = ();
}

while (my $line = <STDIN>) {
    chomp $line;

    if ($line =~ /^\s*$/) {
        flush_sample();
        next;
    }

    if ($line =~ /^(.+?)\s+\d+\s+[\d.]+:\s+(\d+)\s+\S+:/) {
        flush_sample();
        $comm = $1;
        $comm =~ s/\s+$//;
        $count = $2;
        next;
    }

    next unless defined $count;

    next unless $line =~ /^\s*[0-9a-f]+\s+/i;

    my $frame = $line;
    $frame =~ s/^\s*[0-9a-f]+\s+//i;
    my $module_start = rindex($frame, ' (');
    $frame = substr($frame, 0, $module_start) if $module_start >= 0;
    $frame =~ s/\+0x[0-9a-f]+$//i;
    push @frames, $frame if $frame ne '';
}

flush_sample();
