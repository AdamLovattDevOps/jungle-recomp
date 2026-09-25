# Controllers

Any SDL game controller (Xbox, PlayStation, Switch, Steam Deck, Legion Go, Steam Input's
virtual pad) plays every game. The 1995 games only ever read the keyboard and the mouse, so
`src/pad.c` turns the pad into the keys and mouse buttons the scene on screen listens to, and
sends the engine only the changes. Tested by `make pad_test` against an SDL virtual pad.

Everywhere: **Start** is Enter, **Back/Select** is Escape, the **right stick** moves the
pointer, **RT** (or clicking R3) is the left mouse button and **LT** the right one, for the
rules panels and anything else you would click.

| | D-pad / left stick | A | B | X | Y | LB / RB | LT / RT |
|---|---|---|---|---|---|---|---|
| Menus, Options, scores | d-pad: arrows; stick: pointer | click | Esc | Space | Enter | | right / left click |
| Hippo Hop | walk, hop forward/back | jump (X) | run (Z) | jump (Space) | Enter | run (Z) | clicks |
| Burper | move, mondo burp | burp (X) | tail swipe (Z) | burp (Space) | Enter | tail swipe (Z) | clicks |
| Jungle Pinball | up: both flippers; down: shake | launch (Enter) | shake (Space) | shake | launch | left / right flipper | left / right flipper |
| Bug Drop | move, drop faster | rotate ↺ | rotate ↻ | Space | Enter | rotate ↺ / ↻ | clicks |
| Sling Shooter | either stick aims | fire (click) | right click | Space | Enter | | clicks |

The sticks turn into keys with hysteresis (on past half way, off below a third), so a thumb
resting near the edge does not chatter.

## Bug Drop, two players

Bug Drop's players use the game's two redefinable keyboard layouts. The pad reads the live
layout, so keys changed on the Options screen still work.

- **Two pads:** each pad is a whole player (pad 1 Timon, pad 2 Pumbaa).
- **One pad, Joy-Con style:** when a two-player game starts with only one pad connected, the
  pad splits down the middle.

| | Player 1 (Timon), left half | Player 2 (Pumbaa), right half |
|---|---|---|
| move / drop | d-pad or left stick | A B X Y as a d-pad (Y up, A down, X left, B right) or right stick |
| rotate ↺ | LB | RB |
| rotate ↻ | LT | RT |

## High-score names

With a pad the name is entered arcade style: **up/down** rolls the letter in place,
**A** or **right** keeps it (the next letter starts from the one you kept), **B** or **left**
rubs out, **Start** or **Y** finishes and **Back** cancels. A keyboard, including Steam's
on-screen one, types as usual.
