# -*- makefile -*-

LIGHTUP_EXTRA = combi

lightup  : [X] GTK COMMON lightup LIGHTUP_EXTRA lightup-icon|no-icon

lightup  : [G] WINDOWS COMMON lightup LIGHTUP_EXTRA lightup.res|noicon.res

lightupsolver : [U] lightup[STANDALONE_SOLVER] LIGHTUP_EXTRA STANDALONE
lightupsolver : [C] lightup[STANDALONE_SOLVER] LIGHTUP_EXTRA STANDALONE

ALL += lightup[COMBINED] LIGHTUP_EXTRA

!begin gtk
GAMES += lightup
!end

!begin >list.c
    A(lightup) \
!end

!begin >wingames.lst
lightup.exe:Light Up
!end
