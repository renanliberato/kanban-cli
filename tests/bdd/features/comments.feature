Feature: Task Comments
  As a user
  I want to view and add comments on cards
  So that I can have discussions about tasks

  Background:
    Given a board with a card "test" in "To Do"

  Scenario: Detail view shows comments section when empty
    When I launch the application
    And I press Enter
    Then the screen should contain "No comments yet"

  Scenario: Detail view shows comments hint
    When I launch the application
    And I press Enter
    Then the screen should contain "press c"

  Scenario: Add a comment from detail view
    When I launch the application
    And I press Enter
    And I press "c"
    And I type "this is a test comment"
    And I press Enter
    Then the screen should contain "Comment added"
