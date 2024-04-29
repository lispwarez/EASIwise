/*

Needed libraries
  https://arduinojson.org/
  https://github.com/sstaub/NTP
  https://pubsubclient.knolleary.net/



GET:
  http://192.168.4.1/index
    gets json with general settings
    {"hh":"6", "mm":"31", "DD":"23", "MM":"9", "YY":"23", "GT":"45", "GBT":"50", "ST":"0", "Bst":"0", "Hol":"0", "SEE":"1", "WLE":"0", "WVS":"0", "GTE":"0", "EME":"0", "OTE":"0", "SPE":"0", "S1E":"0", "S2E":"0", "GON":"0", "ADO":"0", "Vem":"0", "Vel":"8", "UPD":"0", "PV":"5","Hst":"0000000000", "Hed":"0000000000" }

  http://192.168.4.1/timer
    gets json with schedule information
    {"T1hn":"4", "T1mn":"0", "T1hf":"6", "T1mf":"0", "T1t":"50", "T2hn":"15", "T2mn":"0", "T2hf":"17", "T2mf":"0", "T2t":"50", "T3hn":"1", "T3mn":"0", "T3hf":"3", "T3mf":"0", "T3t":"50", "T4hn":"15", "T4mn":"0", "T4hf":"17", "T4mf":"0", "T4t":"50" }

  http://192.168.4.1/energy
    set json with daily energy usage
    {"D1":"0.00", "D2":"0.00", "D3":"1.15", "D4":"0.00", "D5":"0.00", "D6":"0.00", "D7":"0.00", "D8":"0.00", "D9":"0.00", "D10":"0.00", "D11":"0.00", "D12":"0.00", "D13":"0.00", "D14":"0.00", "D15":"0.00", "D16":"0.00", "D17":"0.00", "D18":"0.00", "D19":"0.00", "D20":"0.00", "D21":"0.00", "D22":"0.00", "D23":"0.00", "D24":"0.00", "D25":"0.00", "D26":"0.00", "D27":"0.00", "D28":"0.00", "D29":"0.00", "D30":"0.00", "D31":"0.00", "TA":"300" }

  http://192.168.4.1/boost    toggles boost mode

  http://192.168.4.1/holiday    toggles holiday mode
  
POST: (GET seems to work too)
  http://192.168.4.1/holdt?Ds="+l+"&Ys="+i.substring(2,4)+"&Ms="+r+"&hs="+n+"&ms="+s+"&De="+p+"&Ye="+x.substring(2,4)+"&Me="+h+"&he="+d+"&me="+j
    set holiday timers
    
  http://192.168.4.1/settimer?T1hn="+n+"&T1mn="+s+"&T1hf="+a+"&T1mf="+d+"&T2hn="+i+"&T2mn="+r+"&T2hf="+b+"&T2mf="+x+"&T3hn="+p+"&T3mn="+g+"&T3hf="+S+"&T3mf="+k+"&T4hn="+u+"&T4mn="+m+"&T4hf="+f+"&T4mf="+v
    sets the geyser times. T1 = weekday mornings, T2 = weekday evening, T3 weekend mornings, T4 weekend evenings

  http://192.168.4.1/settemp?GBT=t
  http://192.168.4.1/settimer?T1t=t&T2t=t&T3t=t&T4t=t
    set geyser max temperature. need to set in both places? (boost temp and timers temp) The app sets both

  http://192.168.4.1/wifi?wapn=ssid&wpw=pass
    set new WiFi SSID and password

  http://192.168.4.1/setdt?DD="+l+"&MM="+r+"&YY="+c.substring(2,4)+"&hh="+t+"&mm="+o
    set the system time/date

  http://192.168.4.1/save?ado={1 or 0}
    set Auto Drop Off

  http://192.168.4.1/save?tar=value
    set the electricity tarrif

  http://192.168.4.1/save?sse={1 or 0}
    unknown ??

  http://192.168.4.1/integ?i0="+e+"&i1="+t+"&i2="+e+"&i3="+t
    unknown ??


*/
