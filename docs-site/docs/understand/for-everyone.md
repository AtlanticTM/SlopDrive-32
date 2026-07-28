---
title: For everyone
description: >-
  What SlopSync means if you simply own a machine: your apps and your machine agree, the stop control always works, and nothing leaves your home.
register: STE
---

# For everyone

This page is for people who own a machine and do not write software. There is
no code on it and nothing to configure.

**SlopSync is a shared language between a machine and the apps you use with
it.** The machine explains itself. The app listens. That is the whole idea,
and everything below is a consequence of it.

## Your app and your machine agree

If your app shows a number, that is the number the machine is using.

Apps used to guess. You moved a slider, the app drew the new value, and the
machine quietly did something slightly different — because it has limits, and
the app did not know them. The two disagreed, and you had no way to tell which
one was right.

Now the machine answers with what it actually did, and every app shows that
answer. If you ask for more than the machine allows, you see the value it
used, not the value you asked for.

## Your apps keep working after a machine update

When your machine gets new firmware, it describes itself again. Your apps read
the new description and carry on.

You do not have to wait for every app you use to be updated first. You do not
have to keep an old firmware version because one app has not caught up.

## New features show up without an app update

If a machine update adds a setting, that setting appears in the apps you
already have, with its correct name, its correct units and its correct safe
range.

There is nothing magic here. The app builds its screen from what the machine
says it has, so a new control is simply a new thing to show.

## One app can talk to different machines

An app written for this language is not written for one brand of machine. It
asks whatever machine it finds what that machine can do, and shows exactly
that.

A machine without a temperature sensor does not claim one. A machine with a
longer travel says so. The app adjusts on its own.

## The stop control always works

**Any device that can reach the machine can stop it.** That includes a device
that is not allowed to control anything else — a spare phone, a tablet showing
a status page, a small remote you never paired.

Stopping is deliberately the one thing that never asks for permission.
Starting is not.

The machine also confirms that it stopped. The device that pressed stop keeps
asking until it sees the machine agree, so a poor connection does not turn a
stop into a maybe.

Every machine that has a physical emergency stop still relies on it first.
This is an extra path, not a replacement for the switch you can hit with your
hand.

## If your phone dies, the machine settles

When an app is driving the machine directly and it goes quiet — the phone
dies, the app crashes, the WiFi drops — the machine does not keep executing
a stream whose sender is gone. It simply stops receiving new instructions, so
it runs out of fresh commands and settles on its own. Nothing on the machine
broadcasts an emergency stop on your behalf; the hardware and software stop
controls covered above are still there if you need them right now.

There is one deliberate exception, and it is the behavior you want. If you
started a pattern that the *machine itself* is running, your screen locking
does not interrupt it. The machine was never depending on your phone; your
phone only pressed start.

## New devices join by an approval you perform

When a new device asks to control your machine, you approve it on hardware you
already hold — your phone, or a page on the machine itself.

You see what is asking before you say yes. Nothing is granted quietly in the
background, and you can take that permission away later from the same place.

A brand-new machine, out of the box, trusts the first device that asks. If you
just unboxed it and powered it on, you are the person holding it, and that is
the point.

## Nothing leaves your home

Your machine talks to your apps over your own network. There is no account to
create, no service to sign into, and no company in the middle.

Concretely: no usage data is collected, nothing is uploaded, and the machine
works exactly the same with your internet connection unplugged. If your router
goes down, your machine and your phone still talk to each other.

The one thing worth knowing: this is designed for your home network, and it is
not built to be reachable from the internet. That is deliberate, and it is
part of why there is nothing to leak.

## What to expect when your machine updates

Your machine restarts, which takes a few seconds. Your apps reconnect on their
own and pick up wherever the machine actually is.

Nothing resumes by itself. If the machine was moving before the update, it
does not start moving again because an app came back. Starting is always
something you do.

## What this does not do

- It does not decide how your machine moves. That is your machine's job, and
  it stays that way.
- It does not replace the app or the interface you already like.
- It is not a security product. It keeps a stranger's device from casually
  taking over on your network. It is not designed to stop somebody who is
  already inside that network with the right tools and the intent to use them.

## If you want to know more

- [How it works](how-it-works.md) — the same ideas, in pictures.
- [Security model](security.md) — what is protected and what is not, stated
  plainly.
