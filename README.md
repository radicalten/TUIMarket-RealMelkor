# TUIMarket

Fetch informations about stocks, currencies and cryptocurrencies in your terminal.
The program uses Yahoo Finance api.

![pic0](./img/img.png)

## Configuration

The program will loads a list of symbols from one of those files :

* ~/.config/tuimarket/symbols
* ~/.tuimarket/symbols
* ~/.tuimarket_symbols

The file will be read one symbol per line.

## Keybindings

* k, up arrow	- scroll up
* j, down arrow	- scroll down
* q, escape	- exit

## Dependencies

* [libcurl][0] - the multiprotocol file transfer library
* [termbox2][1] - terminal rendering library (included in the source)

[0]: https://curl.se/libcurl/
[1]: https://github.com/termbox/termbox2
