# Language model data attribution

`language_model.bin` is a transformed English/Russian subset of **wordfreq 3.1.1**
by Robyn Speer (Elia Robyn Lake): https://github.com/rspeer/wordfreq

Changes made by DiSwitcher: select single-script words, retain Zipf frequencies,
train smoothed character trigram probabilities and a layout-error classifier,
serialize these into the DSLM0001 binary format. Training code is in
`litert_tools/train_local.py` and `litert_tools/local_model.py`.

The derived model data is distributed under **Creative Commons
Attribution-ShareAlike 4.0**: https://creativecommons.org/licenses/by-sa/4.0/
Keep this directory with redistributed model files. Modifications to the model
data must retain attribution and the applicable share-alike license.

wordfreq's code license and upstream data notices are included in
`wordfreq-LICENSE.txt` and `wordfreq-NOTICE.md`. Those notices credit the original
data sources, including Google Books Ngrams, Wikipedia, OPUS/OpenSubtitles,
SUBTLEX, Leeds Internet Corpus and ParaCrawl. SUBTLEX is freely available data;
credit belongs to Marc Brysbaert and the respective SUBTLEX authors as detailed
in the upstream notice.

This model was trained from public frequency data. No DiSwitcher user logs or
personal dictionaries are included in the default build.
