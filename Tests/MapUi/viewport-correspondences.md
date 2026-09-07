# Stationary big-map regression

`viewport-correspondences.txt` contains six sets of SURF point correspondences
extracted from diagnostic session `20260906-151241`, image
`6_state-change-full.png`. Only numeric feature coordinates are retained.
Each set starts with its point count, followed by `cropX cropY mapX mapY` rows.
The crop is 560 by 560 pixels.

The replay varied nearby candidate search centers while keeping the screenshot
fixed. The old projective model produced centers ranging from approximately
(-7016, 1494) to (-6788, 1770), despite 19 to 24 RANSAC inliers. Its distorted
corners also changed the marker scale. Changing the search prior after accepting
each result allowed this error to repeat even with two-frame confirmation.

The zoom/pan model accepts five of these six correspondence sets near
(-6807.6, 1761.3), with less than 0.3 map units of center variation, and rejects
the remaining set. This measures consistency, not independently surveyed map
accuracy. `IMaoOptimizationTests` checks center and scale stability, inlier
support, and the runtime spatial coverage threshold, alongside synthetic
zoom/pan, outlier, perspective, rotation, and insufficient-match cases.
