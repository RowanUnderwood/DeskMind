# Reddit launch (2026-10-01): reactions and lessons

Threads:
- r/LocalLLaMA: https://www.reddit.com/r/LocalLLaMA/comments/1wuxzdg/why_am_i_like_this_full_chat_and_image_generation/
- r/StableDiffusion: https://www.reddit.com/r/StableDiffusion/comments/1wuw8as/
- r/retrobattlestations: https://www.reddit.com/r/retrobattlestations/comments/1wuxrm7/why_am_i_like_this_image_generation_on_a_286/

Reading them: Reddit blocks WebFetch and plain curl, but the RSS feed works
(`https://www.reddit.com/r/<sub>/comments/<id>/.rss?limit=100`, Firefox user agent).
It rate-limits hard: wait about 15 s between requests and retry on 429.
On 2026-10-02 the feeds had 97 LocalLLaMA comments, 40 StableDiffusion comments, and none for retrobattlestations.

## Overall

Mostly warm. Nostalgia, "coolest thing I've seen today", "get it on Hacker News", a request for a gallery.
LocalLLaMA featured it on their Discord. The negative comments were nearly all one point, and it was fair.

## The main criticism: "it's just a terminal"

About ten comments: "dumb terminal", "how is that different from telnet", "Opus 5.5 'locally'",
"reinventing the basic terminal", "twisted the truth in the headline".
The cause was the titles: "image generation **on a 286**" claims something the post then takes back.
On LocalLLaMA, "local" means *on the device*.

- Arguing back made it worse. "First terminal that can show fullscreen 640x200 16 color images on a Tandy"
  got answered with RIPscrip (1992). "Telnet doesn't do saved chats" invited the same.
- Agreeing worked: "Ok it is a bit disingenuous to say ON a Tandy" turned that thread friendly.
- **Next time: say "client" in the title**, e.g. "A 1990 Tandy 286 as a client for my local Qwen + Krea 2".
  The real feats (an unsupported video mode, a GUI in 640K, a single-socket network stack) hold up without the inflated claim.

## Voice: "AI slop writing"

The post text read as generated: bold-lead bullets, "40 year tech gap? No problem!", "BTW".
The replies that got the warmest response were personal: the kid driving 8 hours to rescue the TL/3,
fighting DeskMate's ROM boot loop, the 1000 SX monitor you had to hit.
**Next time: open with a story in your own words and keep the spec list short.**
Put a gallery link (imgur) in the post itself.

## Ideas from the comments

1. **Dithering looked noisy** in screenshots (the pinup). Floyd-Steinberg on smooth gradients gives grain,
   and the 1280x960 export shows hard pixels that the CRT softens.
   - Try Atkinson as the default for portraits and soft subjects (it drops 1/4 of the error, so flat areas stay clean).
   - Add a "CRT look" export (slight horizontal blur and scanlines) for screenshots to share.
2. **A tiny model running on the 286 itself**: the strongest answer to "not on the Tandy".
   llama2.c's ~260K-parameter TinyStories model in int8 is about 260 KB, which fits in the 4 MB EMS.
   Guess: a few seconds per token in fixed point at 10 MHz (not measured).
   Doubles as an offline mode when MindServer is down. Good follow-up post.
3. **Modem handshake sound** on connect, on the SN76496 (Ok-Leg9665). Cheap, and people would love it.
4. CGA as the next target (a natural "part 2"). Doom8088 (https://github.com/FrenkelS/Doom8088) as a side note.
   Someone suggested naming the server MU/TH/ER.

## Ignore

Two "you overestimate your effort" comments, which were outnumbered several times over.
The jokes (Courage the Cowardly Dog, tokens on a sliderule) are just fun.
