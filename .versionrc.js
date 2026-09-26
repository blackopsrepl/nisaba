/*
 * commit-and-tag-version configuration.
 *
 * The single version surface is the one-line VERSION file, which the
 * Makefile banner reads. Changelog links point at the GitHub mirror;
 * releases are pushed to both the local Forgejo and GitHub.
 */
const versionSurface = {
  filename: "VERSION",
  updater: {
    readVersion: (content) => content.trim(),
    writeVersion: (_content, version) => `${version}\n`,
  },
};

module.exports = {
  packageFiles: [versionSurface],
  bumpFiles: [versionSurface],
  tagPrefix: "v",
  releaseCommitMessageFormat: "chore(release): {{currentTag}}",
  commitUrlFormat: "https://github.com/blackopsrepl/nisaba/commit/{{hash}}",
  compareUrlFormat:
    "https://github.com/blackopsrepl/nisaba/compare/{{previousTag}}...{{currentTag}}",
};
