# PPTX Preview Dependencies

The read-only viewer uses Apache POI 5.5.1 and the dependencies pinned in tools/slides-dependencies.json. Original JAR files retain their upstream LICENSE, NOTICE and copyright notices. TwinFile does not modify those libraries. The bridge source is assets/SlideBridge.java.

- Apache POI / OOXML Lite 5.5.1: Apache-2.0, https://poi.apache.org/ and https://github.com/apache/poi.
- XMLBeans 5.3.0: Apache-2.0, https://xmlbeans.apache.org/.
- Apache Commons Compress 1.28.0, IO 2.21.0, Codec 1.20.0, Collections 4.5.0, Math 3.6.1, Lang 3.18.0: Apache-2.0, https://commons.apache.org/.
- Log4j API 2.24.3: Apache-2.0, https://logging.apache.org/log4j/2.x/.
- CurvesAPI 1.08: BSD-3-Clause, https://github.com/virtuald/curvesapi.
- SparseBitSet 1.3: Apache-2.0, https://github.com/brettwooldridge/SparseBitSet.

Exact binaries and corresponding source JARs are available from https://repo.maven.apache.org/maven2/. For each locked coordinate, replace dots in group with slashes, append artifact/version/, then artifact-version.jar or artifact-version-sources.jar. Source JARs contain upstream library source, not TwinFile installers.

The deployed component manifest records each JAR's SHA256. Apache-2.0 text is in licenses/Apache-2.0.txt. Original JAR notices cover additional incorporated material. This file does not relicense upstream dependencies or claim a full legal audit.
