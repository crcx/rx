#!./rx

Forth systems are traditionally interactive. Rx is not; I normally
write my sources and run them from the command line.

This implements an interactive interface for Retro. It's not a great
environment; editing is limited to backspace, no checks are done for
buffer over/underflow.

~~~
{{
  :eol? (c-f)
    [ ASCII:CR eq? ] [ ASCII:LF eq? ] [ ASCII:SPACE eq? ] tri or or ;

  :valid? (s-sf)
    dup s:length n:-zero? ;

  :check-bs (c-c)
    dup [ #8 eq? ] [ #127 eq? ] bi or [ buffer:get buffer:get drop-pair ] if ;

  :c:get (-c) #1 io:scan-for io:invoke ;

  :s:get (-s) [ #1025 buffer:set
                [ c:get dup buffer:add check-bs eol? ] until
                  buffer:start s:chop ] buffer:preserve ;

  :banner
    @Version #100 /mod 'RETRO_12_(%n.%n)\n s:format s:put
    FREE here EOM '%n_Max,_%n_Used,_%n_Free\n s:format s:put ;

  :prompt compiling? [ nl dump-stack '_-->_ s:put ] -if ;
---reveal---
  :listen (-)
    banner
    repeat prompt s:get valid? &interpret &drop choose again ;
}}

listen
~~~
