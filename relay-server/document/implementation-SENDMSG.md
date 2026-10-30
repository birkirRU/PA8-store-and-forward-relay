
## Client Sends 

Server picks it up. Sees the format 

`// SENDMSG,<from>,<to>,<message>   -> "OK"`

stores a newly initialized struct Message in mailboxes_
```cpp
    struct Message {
        std::string from;
        std::string to;
        time_t timestamp;
        std::string text;
    };
```

TODO:

Server then needs to decide whether to *store* or *forward* the message.

forward (requires queue_send call) iff <to> can be found within client_ and is an established connected peer (this removes the message from deque)

store iff the above condition is false.

