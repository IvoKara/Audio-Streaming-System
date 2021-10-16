# Configs
For more information about how to setup
the configuration look up at the Documentation file.
``` 
	bluetooth/
	├── audio.conf -> /etc/bluetooth/audio.conf
	├── main.conf -> /etc/bluetooth/main.conf
	├── network.conf - not neccessary
	├── input.conf - not neccessary
	└── bluetooth.service -> /lib/systemd/system/bluetooth.service

	bluealsa/
	├── asound.conf -> ~/.asoundrc (local) || /etc/asound.conf (global)
	└── bluealsa.service -> /lib/systemd/system/bluealsa.service

	crontab/
	└── crontab -> this file has no specific location
		       copy its content to 'crontab -e'

	wireless/ ? (only to be memorised)
	├── interfaces -> /etc/network/interfaces
	└── wpa\_supplicant.conf -> /etc/wpa\_supplicant/wpa\_supplicant.conf 
```
