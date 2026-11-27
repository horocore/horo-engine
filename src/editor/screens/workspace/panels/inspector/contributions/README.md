# Inspector extension contributions

[InspectorContributionProjection.cpp](InspectorContributionProjection.cpp) prepares restricted
extension data for the inspector. [InspectorContributionView.cpp](InspectorContributionView.cpp)
draws the projection and reports user changes through the attached host.
The [workspace extension handlers](../../../extensions/README.md) own command admission.
