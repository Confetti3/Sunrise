# Claiming a Triumph: Web Service opcode 1801

A record whose requirements are met reads **Ready to Claim** in the Triumphs screen. Clicking it
sends opcode 1801. This build decodes that request and resolves the record it names; it does not yet
change any state, so the entry stays claimable.

## What the client does, measured

Three claims were made in play and traced end to end at debug level:

```
ev=ws stage=request opcode=1801 transaction=0 payload_bytes=3 payload_hex=80DD00
ev=ws1801 stage=claim result=ok reason=decoded record_index=221
ev=transport stage=frame conn=1 type=1 bytes=37
ev=bap svc=10 rsp=11 result=ok
```

The full opcode sequence around them:

```
39391 op104
48358 op1801      claim
52742 op1801      claim
53212 op1801      claim
67824 op701       14 s later, unrelated
```

**The client accepts the reply and asks for nothing else.** No retry, no state fetch, no follow-up
opcode. A grep for push or queuez activity after the claims returns nothing, because the server
sends nothing.

That isolates the gap precisely: the reply shape is already correct, and the client is waiting on a
**push** that never arrives. It is not rejecting the response, so a richer response payload is not
what is missing.

## The request

Three bytes, the same shape as the opcode-1820 Collections pull: a presence bit then a fifteen-bit
record row index.

```
payload_hex=80DD00  ->  0x80DD, presence 1, record row 221
payload_hex=80E300  ->  0x80E3, presence 1, record row 227
payload_hex=80E000  ->  0x80E0, presence 1, record row 224
```

The row indexes the records and lore table `0x81319339`. `opcode1801::parse_request` refuses an
absent record, non-zero padding, a wrong length and a wrong opcode; 13 cases are covered by a
standalone test run against payloads captured from real clicks.

## Why the dispatch hook sits outside the chain

`web_service_runtime.cpp` computes `prepared` from the outcome and answers any **dispatched** opcode
that prepared no mutation with `kRefusedStatus`. A claim prepares nothing yet, so adding 1801 to the
dispatch chain would convert today's silently accepted claim into an explicit refusal -- a
regression wearing the shape of progress. The hook therefore runs before the chain and leaves the
outcome untouched.

Move it into the chain in the same commit that gives it a mutation, not before.

## The records domain

`state::build_data::records` exists so a claim can go from a record row to the bank index it has to
set, without walking the mapping tables per request.

```c
struct Definition {
    uint16_t definitionIndex;      // native record row, what the claim names
    uint16_t completionFlagIndex;  // account flag bank row, or 0xFFFF when unaddressable
};
```

`package_record_build.cpp` reads both tables at extraction time:

- record row `+100` holds the unlock **slot** of the record's completion flag
- the account flag mapping table (root slot 111, descriptor 8) maps a **destination slot** to the
  **row number** whose object byte feeds it

**A slot is not an array index.** The byte that sets slot `s` lives at the row of the mapping table
whose destination is `s`, so the resolution is done once here rather than per claim.

## What is still missing

Only the state transition and its push. Three pieces:

1. **A mutable claimed-record set.** `state::unlocks` is an immutable policy by contract --
   `publish`, `get`, `clear` and nothing else -- so claimed records need their own store.
2. **An encoder change.** The family-4 account encoder has to OR those flags into the account flag
   bank when it builds the object.
3. **A push.** Follow the existing `Pending*` mutation pattern: add a `PendingRecordClaim` to the
   `Outcome::Mutation` variant, prepare it in `claim_record`, and let the established publication
   path carry the new Family-4 version to the client.

## One thing to confirm before building step 2

**It is not established that the completion flag is what marks a record claimed.** Every record
completion flag can be set while the client still offers the claim, which was observed directly over
a long session. What the traced claims prove is the *delivery mechanism* -- a push rather than a
response -- not the payload.

Steps 1 and 3 are needed for any per-record state change and are safe to build. Only step 2 depends
on `+100` being the right field, and if it turns out to be a different one that is a small change at
the end rather than a redesign.
