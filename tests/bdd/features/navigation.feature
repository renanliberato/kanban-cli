Feature: Navigation
  As a user
  I want to navigate between columns and cards using hjkl and arrow keys
  So that I can reach any card on the board

  Scenario: hjkl moves selection between columns
    Given a board with the following cards
      | title  | column |
      | alpha  | To Do  |
      | beta   | Doing  |
      | gamma  | Done   |
    When I launch the application
    # Start in To Do. Press 'l' to move to Doing, then add there.
    When I press "l"
    And I begin adding a card with title "added-to-doing"
    And I confirm the input
    Then the "Doing" column should contain "added-to-doing"
    And the "To Do" column should not contain "added-to-doing"

  Scenario: Arrow keys navigate between columns
    Given a board with no cards
    When I launch the application
    When I press "right arrow"
    And I begin adding a card with title "arrow-right-add"
    And I confirm the input
    Then the "Doing" column should contain "arrow-right-add"

  Scenario: j and down arrow move selection down within a column
    Given a board with cards "one" and "two" in "To Do"
    When I launch the application
    # Selection starts on "one". Press 'j' to get to "two", then delete it.
    When I press "j"
    And I delete the card "two"
    And I confirm the deletion
    Then the "To Do" column should contain "one"
    And the "To Do" column should not contain "two"

  Scenario: k and up arrow move selection up within a column
    Given a board with cards "one" and "two" in "To Do"
    When I launch the application
    # Navigate down then up
    When I press "j"
    When I press "k"
    And I delete the card "one"
    And I confirm the deletion
    Then the "To Do" column should contain "two"
    And the "To Do" column should not contain "one"
