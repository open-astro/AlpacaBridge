# Camera ImageArray and ImageBytes

## Negotiation

The client calls Camera `imagearray`. To request binary transfer it includes:

`Accept: application/imagebytes`

A supporting device returns `Content-Type: application/imagebytes`. A device that does not use ImageBytes returns the ordinary JSON representation with `Content-Type: application/json`. Clients must branch on the response content type.

Do not return binary data without the client advertising support. Do not label JSON as ImageBytes or binary data as JSON.

## Payload structure

An ImageBytes response contains:

1. a fixed 44-byte metadata block of eleven 32-bit fields; and
2. image data on success, or a UTF-8 error message on failure.

| Bytes | Field | Meaning |
| ---: | --- | --- |
| 0–3 | `MetadataVersion` | `1` for this baseline |
| 4–7 | `ErrorNumber` | Zero on success; Alpaca error on failure |
| 8–11 | `ClientTransactionID` | Client transaction ID |
| 12–15 | `ServerTransactionID` | Server transaction ID |
| 16–19 | `DataStart` | Byte offset where data begins |
| 20–23 | `ImageElementType` | Source array element type |
| 24–27 | `TransmissionElementType` | Type used on the wire |
| 28–31 | `Rank` | 2 or 3 |
| 32–35 | `Dimension1` | Width / first dimension |
| 36–39 | `Dimension2` | Height / second dimension |
| 40–43 | `Dimension3` | Third dimension, or 0 for rank 2 |

Metadata integers and image integer elements use little-endian byte order. Metadata version, error number, data offset, type codes, rank, and dimensions must not be negative.

## Element type codes

| Code | Type |
| ---: | --- |
| 0 | Unknown |
| 1 | Int16 |
| 2 | Int32 |
| 3 | Double |
| 4 | Single |
| 5 | UInt64 |
| 6 | Byte |
| 7 | Int64 |
| 8 | UInt16 |
| 9 | UInt32 |

Choose a transmission type that can represent every returned value without truncation or sign corruption. Validate multiplication and buffer-size arithmetic against integer overflow before allocating or serializing.

## Shape and ordering

- Monochrome and Bayer data: `Array[NumX, NumY]`, rank 2.
- RGB data: `Array[NumX, NumY, ColourPlane]`, rank 3; planes 0, 1, 2 represent red, green, blue.
- Serialize with the rightmost index changing fastest.

Rank 2 order:

```text
for x in 0..NumX-1
  for y in 0..NumY-1
    emit image[x,y]
```

Rank 3 order:

```text
for x in 0..NumX-1
  for y in 0..NumY-1
    for plane in 0..2
      emit image[x,y,plane]
```

This is counterintuitive for implementations whose internal frame is stored Y-major. Transpose or stride deliberately; never reinterpret a row-major vendor buffer without proving the public ASCOM ordering.

## Success and failure

On success, `ErrorNumber=0` and bytes from `DataStart` are serialized image elements. On failure, use a nonzero Alpaca error and place the UTF-8 error message at `DataStart`. Clients read metadata before interpreting the remaining bytes.

## Validation

Test:

- JSON fallback and ImageBytes negotiation;
- 2D and 3D index ordering using asymmetric dimensions and unique values;
- every transmission type used by the implementation;
- little-endian encoding;
- transaction IDs and errors;
- malformed dimensions and overflow resistance;
- full payload length and `DataStart` consistency;
- large real frames for memory use and response-time targets.

