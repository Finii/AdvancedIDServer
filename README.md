# AdvancedIDServer
Making EventID readout more robust

What does AIDS do?
==================
It connects to the TCP based event ID server and continuously gets the Ethernet packets with the IDs. These packets are time stamped with microsecond resolution and stored. This runs all the time. Because we have a lot of data we can correlate the local time on that machine (that generated the local time stamps) and the IDs. If the Ethernet connection lags sometimes, chokes, hick ups and messes around with our packets, even drops some... Who cares. By closely monitoring all data we can define a function to calculate the event ID from the local time on that machine.

The users can access this server via a socket. This time we use localhost:58051. Connect your program to that socket and you get immediately the actual event ID for this very moment. The packet you get looks like this:

73657523.41652 O 354

More abstract you get a number, dot, a second number. A character. A third number.

The first number is the event ID (decimal). The number after the dot is the fractional part of the event ID. Like how far after the light hit the chamber did you query. Thus you can estimate your timing. So in fact you get the event ID as float.

The char gives basic information about the state of the AIDS. 
O means Okay, all is well
S means Stale, our database is quite old and if the state does not change to O soon something is wrong.
D means Disconnected, we get no new information. Hopefully it will change to O in a moment.

So O means no worries. S and D are warnings, if they stay for more than some seconds your timing might run off.

The last number is the statistical quality of our calculation. Interpret it like something equal to timing jitter with the unit of microseconds. You can use it to monitor your Ethernet link quality. 10000 means 10 ms, everything below will not affect anyone I assume.

You get a new calculated ID every time you send any data to the server. So normally your program looks like this and should not close and open the socket all the time:

Connect to AIDS.
Check the state flag and jitter number in the first packet. Ignore the ID.
loop
  do your measurement
  get a trigger
  send something to AIDS and get the event ID
  save your aquired data with that ID
until shift finished or FLASH beam lost

Have fun
  Fini
