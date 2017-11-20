# Pulseaudio RTP source

# Build:
```
$ gcc -Wall -o partpd partpd.c
```

# Usage:
RTP proxy:
```
$ partpd <stream-port> <pulseaudio-pipe-path>
```

Pulseaudio source configuration:
```
$ pactl load-module module-pipe-source file=<pulseaudio-pipe-path> format=s16be rate=44100 channels=2 source_name=partpd_source
```

RTP sender example (ffmpeg):
```
$ ffmpeg -re -i <input> -acodec pcm_s16be -ar 44100 -ac 2 -f rtp rtp://<partpd-host>:<partpd-port>
```
